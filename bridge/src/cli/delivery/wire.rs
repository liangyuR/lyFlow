use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::Path;
use std::time::{Duration, Instant};

const MAGIC: &[u8; 8] = b"LYFPUB1\0";
const MAX_HEADER: usize = 2 * 1024 * 1024;
const MAX_ACK: usize = 65536;
const POLL: Duration = Duration::from_millis(50);

#[derive(Debug)]
pub(super) struct Error {
    pub kind: &'static str,
    pub message: String,
    pub unconfirmed: bool,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{}: {}{}",
            self.kind,
            self.message,
            if self.unconfirmed {
                "; delivery is unconfirmed"
            } else {
                ""
            }
        )
    }
}

fn error(kind: &'static str, message: impl Into<String>, unconfirmed: bool) -> Error {
    Error {
        kind,
        message: message.into(),
        unconfirmed,
    }
}

fn check(deadline: Instant, cancelled: &impl Fn() -> bool, unconfirmed: bool) -> Result<(), Error> {
    if cancelled() {
        return Err(error("cancelled", "publication cancelled", unconfirmed));
    }
    if Instant::now() >= deadline {
        return Err(error(
            "timeout",
            "publication deadline exceeded",
            unconfirmed,
        ));
    }
    Ok(())
}

fn endpoint(value: &str) -> Result<SocketAddr, Error> {
    value
        .strip_prefix("tcp://")
        .ok_or_else(|| {
            error(
                "invalid_endpoint",
                "expected tcp://<numeric-IP>:<port>",
                false,
            )
        })?
        .parse::<SocketAddr>()
        .map_err(|_| {
            error(
                "invalid_endpoint",
                "expected numeric IPv4 or bracketed IPv6 and port; DNS resolution is not implicit",
                false,
            )
        })
        .and_then(|address| {
            if address.port() == 0 {
                Err(error(
                    "invalid_endpoint",
                    "endpoint port must be nonzero",
                    false,
                ))
            } else {
                Ok(address)
            }
        })
}

fn connect(
    address: SocketAddr,
    deadline: Instant,
    cancelled: &impl Fn() -> bool,
) -> Result<TcpStream, Error> {
    loop {
        check(deadline, cancelled, false)?;
        match TcpStream::connect_timeout(&address, remaining(deadline, false)?) {
            Ok(stream) => return Ok(stream),
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
                ) => {}
            Err(e) => return Err(error("connect_failed", e.to_string(), false)),
        }
    }
}

fn remaining(deadline: Instant, sent: bool) -> Result<Duration, Error> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .map(|d| d.min(POLL))
        .ok_or_else(|| error("timeout", "publication deadline exceeded", sent))
}

fn write(
    stream: &mut TcpStream,
    mut data: &[u8],
    deadline: Instant,
    cancelled: &impl Fn() -> bool,
    sent: &mut bool,
) -> Result<(), Error> {
    while !data.is_empty() {
        check(deadline, cancelled, *sent)?;
        stream
            .set_write_timeout(Some(remaining(deadline, *sent)?))
            .map_err(|e| error("io", e.to_string(), *sent))?;
        match stream.write(data) {
            Ok(0) => {
                return Err(error(
                    "disconnected",
                    "peer closed during publication",
                    *sent,
                ))
            }
            Ok(n) => {
                *sent = true;
                data = &data[n..];
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(error("io", e.to_string(), *sent)),
        }
    }
    Ok(())
}

pub(super) fn publish(
    path: &Path,
    metadata: &Value,
    endpoint_text: &str,
    topic: &str,
    timeout: Duration,
    max_bytes: u64,
    cancelled: impl Fn() -> bool,
) -> Result<Value, Error> {
    if timeout.is_zero() || timeout > Duration::from_secs(3600) {
        return Err(error(
            "invalid_timeout",
            "timeout must be within (0, 3600] seconds",
            false,
        ));
    }
    if topic.is_empty() || topic.len() > 512 || topic.chars().any(char::is_control) {
        return Err(error(
            "invalid_topic",
            "topic must be nonempty, <=512 bytes and contain no control characters",
            false,
        ));
    }
    let frame_id = metadata["frameId"]
        .as_str()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| error("invalid_metadata", "frameId is required", false))?;
    let address = endpoint(endpoint_text)?;
    let deadline = Instant::now() + timeout;
    check(deadline, &cancelled, false)?;
    let mut file = File::open(path).map_err(|e| error("archive", e.to_string(), false))?;
    file.try_lock_shared()
        .map_err(|e| error("archive_busy", e.to_string(), false))?;
    let info = file
        .metadata()
        .map_err(|e| error("archive", e.to_string(), false))?;
    let bytes = info.len();
    if !info.is_file() || bytes == 0 || bytes > max_bytes {
        return Err(error(
            "archive_budget",
            "archive must be a nonempty regular file within max-bytes",
            false,
        ));
    }
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 65536];
    let mut counted = 0u64;
    loop {
        check(deadline, &cancelled, false)?;
        let n = file
            .read(&mut buffer)
            .map_err(|e| error("archive", e.to_string(), false))?;
        if n == 0 {
            break;
        }
        counted = counted
            .checked_add(n as u64)
            .ok_or_else(|| error("archive_budget", "archive size overflow", false))?;
        if counted > bytes {
            return Err(error(
                "archive_changed",
                "archive changed during publication preparation",
                false,
            ));
        }
        hash.update(&buffer[..n]);
    }
    if counted != bytes {
        return Err(error(
            "archive_changed",
            "archive changed during publication preparation",
            false,
        ));
    }
    use std::io::Seek;
    file.rewind()
        .map_err(|e| error("archive", e.to_string(), false))?;
    let digest = format!("{:x}", hash.finalize());
    let header = json!({"type":"lyflow.Publish.v1","schemaVersion":1,"topic":topic,"frameId":frame_id,"metadata":metadata,"payloadFormat":"lyflow.FrameArchive.v1","payloadBytes":bytes.to_string(),"payloadSha256":digest});
    let header_bytes =
        serde_json::to_vec(&header).map_err(|e| error("metadata", e.to_string(), false))?;
    if header_bytes.len() > MAX_HEADER {
        return Err(error(
            "metadata_budget",
            "publication header exceeds 2 MiB",
            false,
        ));
    }
    let mut stream = connect(address, deadline, &cancelled)?;
    stream
        .set_nodelay(true)
        .map_err(|e| error("io", e.to_string(), false))?;
    let mut sent = false;
    write(&mut stream, MAGIC, deadline, &cancelled, &mut sent)?;
    write(
        &mut stream,
        &(header_bytes.len() as u32).to_le_bytes(),
        deadline,
        &cancelled,
        &mut sent,
    )?;
    write(
        &mut stream,
        &bytes.to_le_bytes(),
        deadline,
        &cancelled,
        &mut sent,
    )?;
    write(&mut stream, &header_bytes, deadline, &cancelled, &mut sent)?;
    let mut transferred = 0u64;
    let mut sent_hash = Sha256::new();
    while transferred < bytes {
        check(deadline, &cancelled, sent)?;
        let remaining = (bytes - transferred).min(buffer.len() as u64) as usize;
        let n = file
            .read(&mut buffer[..remaining])
            .map_err(|e| error("archive", e.to_string(), sent))?;
        if n == 0 {
            return Err(error(
                "archive_changed",
                "archive ended during publication",
                sent,
            ));
        }
        sent_hash.update(&buffer[..n]);
        write(&mut stream, &buffer[..n], deadline, &cancelled, &mut sent)?;
        transferred += n as u64;
    }
    if format!("{:x}", sent_hash.finalize()) != digest {
        return Err(error(
            "archive_changed",
            "archive bytes changed during publication",
            true,
        ));
    }
    let mut ack = Vec::new();
    loop {
        check(deadline, &cancelled, true)?;
        stream
            .set_read_timeout(Some(remaining(deadline, true)?))
            .map_err(|e| error("io", e.to_string(), true))?;
        let mut chunk = [0u8; 512];
        match stream.read(&mut chunk) {
            Ok(0) => {
                return Err(error(
                    "disconnected",
                    "peer closed without a matching acknowledgement",
                    true,
                ))
            }
            Ok(n) => {
                ack.extend_from_slice(&chunk[..n]);
                if ack.len() > MAX_ACK {
                    return Err(error("ack_budget", "acknowledgement exceeds64 KiB", true));
                }
                if let Some(end) = ack.iter().position(|c| *c == b'\n') {
                    if ack[end + 1..].iter().any(|b| !b.is_ascii_whitespace()) {
                        return Err(error(
                            "invalid_ack",
                            "unexpected bytes after acknowledgement",
                            true,
                        ));
                    }
                    let value: Value = serde_json::from_slice(&ack[..end])
                        .map_err(|e| error("invalid_ack", e.to_string(), true))?;
                    if value["type"] != "lyflow.PublishAck.v1"
                        || value["schemaVersion"] != 1
                        || value["frameId"] != frame_id
                        || value.get("session").filter(|s| !s.is_null())
                            != metadata.get("session").filter(|s| !s.is_null())
                    {
                        return Err(error(
                            "mismatched_ack",
                            "acknowledgement version/frame/session/epoch/sequence differs",
                            true,
                        ));
                    }
                    if value["accepted"] != true {
                        return Err(error(
                            "rejected",
                            value["message"]
                                .as_str()
                                .unwrap_or("receiver rejected the frame"),
                            false,
                        ));
                    }
                    return Ok(
                        json!({"type":"lyflow.PublishReceipt.v1","frameId":frame_id,"session":metadata.get("session"),"topic":topic,"endpoint":endpoint_text,"payloadBytes":bytes.to_string(),"payloadSha256":digest,"acknowledged":true}),
                    );
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(error("io", e.to_string(), true)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;
    use std::sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc,
    };
    fn archive() -> std::path::PathBuf {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "lyflow-p6-wire-{}-{}.bin",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let mut f = std::fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .unwrap();
        f.write_all(&[0, 255, 128, 4, 0, 9, 1]).unwrap();
        path
    }
    fn receive(stream: &mut TcpStream) -> Value {
        let mut magic = [0; 8];
        stream.read_exact(&mut magic).unwrap();
        assert_eq!(&magic, MAGIC);
        let mut h = [0; 4];
        stream.read_exact(&mut h).unwrap();
        let mut n = [0; 8];
        stream.read_exact(&mut n).unwrap();
        let mut data = vec![0; u32::from_le_bytes(h) as usize];
        stream.read_exact(&mut data).unwrap();
        let value: Value = serde_json::from_slice(&data).unwrap();
        let mut payload = vec![0; u64::from_le_bytes(n) as usize];
        stream.read_exact(&mut payload).unwrap();
        assert_eq!(payload, [0, 255, 128, 4, 0, 9, 1]);
        assert_eq!(value["payloadBytes"], "7");
        assert_eq!(
            value["payloadSha256"],
            format!("{:x}", Sha256::digest(&payload))
        );
        value
    }
    fn ack(value: &Value) -> Vec<u8> {
        format!("{}\n",json!({"type":"lyflow.PublishAck.v1","schemaVersion":1,"frameId":value["frameId"],"session":value["metadata"]["session"],"accepted":true})).into_bytes()
    }
    #[test]
    fn real_tcp_publication_preserves_binary_and_requires_matching_session_epoch_and_frame_ack() {
        for (mismatch, session) in [(false, true), (true, true), (false, false)] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = format!("tcp://{}", listener.local_addr().unwrap());
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                let mut value = receive(&mut stream);
                if mismatch {
                    value["metadata"]["session"]["epoch"] = json!("wrong");
                }
                stream.write_all(&ack(&value)).unwrap();
            });
            let path = archive();
            let meta = if session {
                json!({"frameId":"f","session":{"id":"s","epoch":"2","sequence":"3"}})
            } else {
                json!({"frameId":"f"})
            };
            let result = publish(
                &path,
                &meta,
                &address,
                "work/results",
                Duration::from_secs(2),
                100,
                || false,
            );
            if mismatch {
                assert_eq!(result.unwrap_err().kind, "mismatched_ack");
            } else {
                assert_eq!(result.unwrap()["acknowledged"], true);
            }
            server.join().unwrap();
            std::fs::remove_file(path).unwrap();
        }
    }
    #[test]
    fn disconnect_timeout_and_cancel_are_unconfirmed_without_automatic_frame_retry() {
        for mode in ["disconnect", "timeout", "cancel"] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = format!("tcp://{}", listener.local_addr().unwrap());
            let cancelled = Arc::new(AtomicBool::new(false));
            let signal = Arc::clone(&cancelled);
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                receive(&mut stream);
                match mode {
                    "disconnect" => {}
                    "cancel" => {
                        signal.store(true, Ordering::SeqCst);
                        std::thread::sleep(Duration::from_millis(150));
                    }
                    _ => std::thread::sleep(Duration::from_millis(250)),
                };
                listener.set_nonblocking(true).unwrap();
                assert!(
                    matches!(listener.accept(),Err(e) if e.kind()==std::io::ErrorKind::WouldBlock)
                );
            });
            let path = archive();
            let meta = json!({"frameId":"f"});
            let result = publish(
                &path,
                &meta,
                &address,
                "results",
                Duration::from_millis(100),
                100,
                || cancelled.load(Ordering::SeqCst),
            )
            .unwrap_err();
            assert!(result.unconfirmed);
            assert_eq!(
                result.kind,
                match mode {
                    "disconnect" => "disconnected",
                    "cancel" => "cancelled",
                    _ => "timeout",
                }
            );
            server.join().unwrap();
            std::fs::remove_file(path).unwrap();
        }
        assert!(endpoint("tcp://hostname.example:123").is_err());
        assert!(endpoint("http://127.0.0.1:123").is_err());
        assert!(endpoint("tcp://127.0.0.1:0").is_err());
    }
}
