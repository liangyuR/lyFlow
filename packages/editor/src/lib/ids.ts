// id 生成。节点/边 id 只需单图内唯一，短且可读比全局唯一重要（它们进 git diff）。
// 图本身的 id 用 ULID：单调递增、可排序、重命名文件后仍能追踪同一张图。

const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32，去掉 ILOU

function randomChars(n: number): string {
  const bytes = new Uint8Array(n);
  crypto.getRandomValues(bytes);
  let out = "";
  for (const b of bytes) out += ALPHABET[b % ALPHABET.length];
  return out;
}

/** ULID：48 位时间戳 + 80 位随机。同一毫秒内创建的图也不会撞。 */
export function newDocId(): string {
  let time = Date.now();
  let ts = "";
  for (let i = 0; i < 10; i++) {
    ts = ALPHABET[time % 32] + ts;
    time = Math.floor(time / 32);
  }
  return ts + randomChars(16);
}

/** 单图内唯一的短 id。`taken` 传入已用 id 以避免碰撞 ——
 *  粘贴时会一次生成很多 id，靠随机性赌不撞是不够的。 */
export function newLocalId(prefix: string, taken: ReadonlySet<string>): string {
  for (let attempt = 0; attempt < 1000; attempt++) {
    const id = `${prefix}_${randomChars(6).toLowerCase()}`;
    if (!taken.has(id)) return id;
  }
  // 实际上到不了这里；到了说明 taken 大到不正常，用时间戳兜底
  return `${prefix}_${Date.now().toString(36)}`;
}
