// transform 参数的值（docs/operator-manifest.md「transform 与 curve 的值」）：16 个数的 4×4 **行主序**矩阵，
// 与 core 的 Transform 数据类型同一个布局 —— 平移在 m[3]、m[7]、m[11]，最后一行是 0 0 0 1。
// 控件显示成「平移 xyz + 旋转 xyz（度）」，旋转是内旋 X→Y→Z，即 R = Rz·Ry·Rx，与 transform.make 同一约定。
// 纯函数，不碰 DOM（node --test 直接测）。

export const IDENTITY: readonly number[] = Object.freeze([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

export interface TR {
  /** 平移，参数自己的单位（通常是米）。 */
  t: [number, number, number];
  /** 欧拉角，度。 */
  r: [number, number, number];
}

const DEG = Math.PI / 180;

/** 12 位有效数字、贴近 0 的抹成 0：cos(90°) 这种 6e-17 写进文件只是噪声。 */
export function clean(v: number): number {
  if (Math.abs(v) < 1e-12) return 0;
  return Number.parseFloat(v.toPrecision(12));
}

/** 取一份能用的 16 个数。形状不对（老文件、手改坏了）退回单位阵，控件不崩。 */
export function asMatrix(value: unknown): number[] {
  if (Array.isArray(value) && value.length === 16 && value.every((v) => typeof v === "number" && Number.isFinite(v))) {
    return value as number[];
  }
  return [...IDENTITY];
}

/** 由平移与欧拉角合成刚体矩阵。 */
export function compose({ t, r }: TR): number[] {
  const [rx, ry, rz] = r.map((d) => d * DEG) as [number, number, number];
  const cx = Math.cos(rx), sx = Math.sin(rx);
  const cy = Math.cos(ry), sy = Math.sin(ry);
  const cz = Math.cos(rz), sz = Math.sin(rz);
  const m = [
    cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, t[0],
    sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, t[1],
    -sy, cy * sx, cy * cx, t[2],
    0, 0, 0, 1,
  ];
  return m.map(clean);
}

/** 是不是刚体变换：左上 3×3 是正交且行列式为 +1、最后一行是 0 0 0 1。
 *  带缩放或切变的矩阵拆不成 T·R，控件只让它在矩阵视图里改。 */
export function isRigid(m: readonly number[], tol = 1e-6): boolean {
  if (m.length !== 16) return false;
  if (Math.abs(m[12]!) > tol || Math.abs(m[13]!) > tol || Math.abs(m[14]!) > tol || Math.abs(m[15]! - 1) > tol) {
    return false;
  }
  const row = (i: number) => [m[i * 4]!, m[i * 4 + 1]!, m[i * 4 + 2]!];
  const rows = [row(0), row(1), row(2)];
  for (let i = 0; i < 3; i += 1) {
    for (let j = 0; j < 3; j += 1) {
      const dot = rows[i]!.reduce((s, v, k) => s + v * rows[j]![k]!, 0);
      if (Math.abs(dot - (i === j ? 1 : 0)) > tol) return false;
    }
  }
  const [a, b, c] = rows as [number[], number[], number[]];
  const det =
    a[0]! * (b[1]! * c[2]! - b[2]! * c[1]!) -
    a[1]! * (b[0]! * c[2]! - b[2]! * c[0]!) +
    a[2]! * (b[0]! * c[1]! - b[1]! * c[0]!);
  return Math.abs(det - 1) <= tol;
}

/** compose 的逆。万向锁（ry = ±90°）时 rx 与 rz 只差一个和，按惯例把 rx 取 0。 */
export function decompose(m: readonly number[]): TR {
  const t: [number, number, number] = [clean(m[3]!), clean(m[7]!), clean(m[11]!)];
  const m20 = Math.max(-1, Math.min(1, m[8]!));
  const ry = Math.asin(-m20);
  let rx: number;
  let rz: number;
  if (Math.abs(m20) < 1 - 1e-9) {
    rx = Math.atan2(m[9]!, m[10]!);
    rz = Math.atan2(m[4]!, m[0]!);
  } else {
    rx = 0;
    rz = Math.atan2(-m[1]!, m[5]!);
  }
  // 角度按 1e-6° 取整：文件里的矩阵常是十位小数（0.9659258263），原样反算是 14.9999999997°
  const deg = (v: number) => clean(Math.round((v / DEG) * 1e6) / 1e6);
  return { t, r: [deg(rx), deg(ry), deg(rz)] };
}

/** 面板行上的一行摘要：T[x, y, z] R[rx, ry, rz]°；不是刚体时说一声。 */
export function summarize(value: unknown): string {
  const m = asMatrix(value);
  if (!isRigid(m)) return "4×4 矩阵（含缩放或切变）";
  const { t, r } = decompose(m);
  const f = (v: number) => String(Number(v.toPrecision(4)));
  return `T[${t.map(f).join(", ")}] R[${r.map(f).join(", ")}]°`;
}
