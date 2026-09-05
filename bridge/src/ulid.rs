//! 极简 ULID 生成器：48 位毫秒时间戳 + 80 位随机，Crockford base32，26 个字符。
//! 熵来自 RandomState 加一个自增计数器 —— 同毫秒不重复比密码学强度重要。

use std::hash::{BuildHasher, Hasher, RandomState};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const CROCKFORD: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

pub fn new() -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);

    let ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
        & 0x0000_FFFF_FFFF_FFFF;

    let seq = COUNTER.fetch_add(1, Ordering::Relaxed);
    let mut hasher = RandomState::new().build_hasher();
    hasher.write_u64(seq);
    hasher.write_u64(ms);
    let rand_hi = hasher.finish();
    let mut hasher2 = RandomState::new().build_hasher();
    hasher2.write_u64(rand_hi ^ seq);
    let rand_lo = hasher2.finish();

    // 128 位：时间戳 48 + 随机 80
    let hi: u64 = (ms << 16) | (rand_hi >> 48);
    let lo: u64 = (rand_hi << 16) | (rand_lo & 0xFFFF);

    let bytes: [u8; 16] = {
        let mut b = [0u8; 16];
        b[..8].copy_from_slice(&hi.to_be_bytes());
        b[8..].copy_from_slice(&lo.to_be_bytes());
        b
    };

    // 26 个 base32 字符 = 130 位，最高位那 2 位补零。
    let mut out = String::with_capacity(26);
    let mut bit = 0usize;
    for _ in 0..26 {
        let mut v = 0u8;
        for _ in 0..5 {
            let b = if bit < 2 {
                0
            } else {
                let i = bit - 2;
                (bytes[i / 8] >> (7 - (i % 8))) & 1
            };
            v = (v << 1) | b;
            bit += 1;
        }
        out.push(CROCKFORD[v as usize] as char);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn is_26_crockford_chars() {
        let id = new();
        assert_eq!(id.len(), 26, "{id}");
        assert!(id.bytes().all(|c| CROCKFORD.contains(&c)), "{id}");
    }

    /// 同一毫秒内连续生成必须不重复 —— 这正是 run id 的实际用法
    /// （抢占式运行时用户可能在一帧里连点两次）。
    #[test]
    fn ten_thousand_ids_are_unique() {
        let set: HashSet<String> = (0..10_000).map(|_| new()).collect();
        assert_eq!(set.len(), 10_000);
    }

    #[test]
    fn is_time_ordered() {
        let a = new();
        std::thread::sleep(std::time::Duration::from_millis(3));
        let b = new();
        // 前 10 个字符是时间戳部分，字典序即时间序
        assert!(a[..10] < b[..10], "{a} vs {b}");
    }
}
