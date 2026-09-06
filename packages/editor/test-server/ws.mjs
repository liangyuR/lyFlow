// 极简 WebSocket 服务端：够用来往浏览器推 JSON 文本帧。零依赖，
// 与 scripts/e2e/cdp.mjs 的取舍一样 —— 桩服务器不该带一棵依赖树。

import crypto from "node:crypto";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function accept(key) {
  return crypto
    .createHash("sha1")
    .update(key + GUID)
    .digest("base64");
}

function frame(text) {
  const payload = Buffer.from(text, "utf8");
  const n = payload.length;
  let header;
  if (n < 126) {
    header = Buffer.from([0x81, n]);
  } else if (n < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 126;
    header.writeUInt16BE(n, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(n), 2);
  }
  return Buffer.concat([header, payload]);
}

/** 只解析出「客户端发了 close / ping」这两件事，载荷一律丢掉。 */
function readOpcodes(buf) {
  const out = [];
  let i = 0;
  while (i + 2 <= buf.length) {
    const opcode = buf[i] & 0x0f;
    const masked = (buf[i + 1] & 0x80) !== 0;
    let len = buf[i + 1] & 0x7f;
    let offset = i + 2;
    if (len === 126) {
      if (offset + 2 > buf.length) break;
      len = buf.readUInt16BE(offset);
      offset += 2;
    } else if (len === 127) {
      if (offset + 8 > buf.length) break;
      len = Number(buf.readBigUInt64BE(offset));
      offset += 8;
    }
    if (masked) offset += 4;
    if (offset + len > buf.length) break;
    out.push(opcode);
    i = offset + len;
  }
  return out;
}

export class WsHub {
  #clients = new Set();

  /** 挂到 http server 的 upgrade 事件上。authorize 返回 false 就拒握手。 */
  attach(server, path, authorize) {
    server.on("upgrade", (req, socket) => {
      const url = new URL(req.url ?? "/", "http://localhost");
      if (url.pathname !== path) {
        socket.destroy();
        return;
      }
      const protocols = (req.headers["sec-websocket-protocol"] ?? "")
        .split(",")
        .map((p) => p.trim())
        .filter(Boolean);
      if (!authorize(protocols)) {
        socket.write("HTTP/1.1 401 Unauthorized\r\n\r\n");
        socket.destroy();
        return;
      }
      const key = req.headers["sec-websocket-key"];
      if (!key) {
        socket.destroy();
        return;
      }
      const lines = [
        "HTTP/1.1 101 Switching Protocols",
        "Upgrade: websocket",
        "Connection: Upgrade",
        `Sec-WebSocket-Accept: ${accept(key)}`,
      ];
      if (protocols.includes("lyflow.v1")) lines.push("Sec-WebSocket-Protocol: lyflow.v1");
      socket.write(`${lines.join("\r\n")}\r\n\r\n`);
      socket.setNoDelay(true);
      this.#clients.add(socket);
      socket.on("data", (chunk) => {
        for (const opcode of readOpcodes(chunk)) {
          if (opcode === 0x8) socket.destroy();
          else if (opcode === 0x9) socket.write(Buffer.from([0x8a, 0x00]));
        }
      });
      const drop = () => this.#clients.delete(socket);
      socket.on("close", drop);
      socket.on("error", drop);
    });
  }

  broadcast(value) {
    const buf = frame(JSON.stringify(value));
    for (const socket of this.#clients) {
      if (socket.writable) socket.write(buf);
    }
  }

  get size() {
    return this.#clients.size;
  }

  closeAll() {
    for (const socket of this.#clients) socket.destroy();
    this.#clients.clear();
  }
}
