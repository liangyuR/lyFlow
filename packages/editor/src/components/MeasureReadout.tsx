// 测量的读数框（docs/measure-plan.md M6）：右下角固定一小块，鼠标穿过它落到画布上（只有「清除」接鼠标）。
// 主预览与连线查看器共用。

import { measureLines, pickCount, type Measure } from "../lib/pick";

export function MeasureReadout({
  measure,
  mode,
  compare = false,
  pointCount,
  onClear,
}: {
  measure: Measure;
  mode: "3d" | "2d";
  /** 对比模式：点位后面标 A / B。 */
  compare?: boolean;
  /** 画面上显示的点数（拾取只吸附到它们）。 */
  pointCount: number;
  onClear(): void;
}) {
  const n = pickCount(measure);
  return (
    <div
      className="measure-readout"
      data-testid="measure-readout"
      title={`拾取只吸附到画面上显示的 ${pointCount.toLocaleString()} 点（抽稀后的），不向后端要全分辨率`}
    >
      {measureLines(measure, mode, compare).map((l) => (
        <div key={l.key} className="measure-readout__row" data-key={l.key}>
          <span className="measure-readout__label">{l.label}</span>
          <span className="measure-readout__value">{l.text}</span>
        </div>
      ))}
      {measure.stale && (
        <div className="measure-readout__stale" data-testid="measure-stale">
          云已更新，点位是之前选的
        </div>
      )}
      <div className="measure-readout__foot">
        <span>{n === 0 ? "单击画面选一个点" : n === 1 ? "再点一个点量距离" : "再点一次重新开始"}</span>
        <button
          type="button"
          className="viewer__btn"
          data-testid="measure-clear"
          disabled={n === 0}
          onClick={onClear}
        >
          清除
        </button>
      </div>
    </div>
  );
}
