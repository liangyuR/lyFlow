// 换了一片云要不要重新取景。同一个坐标系里（两个包围盒相交、对角线差不到 4 倍）就不动相机：调参数重跑、
// 在链上逐个点节点时，视角连同双击设好的转心都留着；平移了 100 m、放大了 1000 倍、完全不相交的才重新取景。
// 不用「新云的中心在不在视野里」作判据：放大到云的一角时中心在视野外，照样会复位。

/** a、b 是 [minx, miny, minz, maxx, maxy, maxz]。 */
export function sameFrame(a: ArrayLike<number>, b: ArrayLike<number>): boolean {
  for (let i = 0; i < 3; i += 1) {
    if (a[i]! > b[i + 3]! || b[i]! > a[i + 3]!) return false;
  }
  const diag = (x: ArrayLike<number>) => Math.hypot(x[3]! - x[0]!, x[4]! - x[1]!, x[5]! - x[2]!);
  const da = diag(a);
  const db = diag(b);
  // 退化成一个点的云：两边都是点（又相交）才算同一个
  if (!(da > 0) || !(db > 0)) return da === db;
  const ratio = da / db;
  return ratio >= 0.25 && ratio <= 4;
}
