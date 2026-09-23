#!/usr/bin/env python
"""A/B：拿 dataset.yml 的每个样本生成一张 LyFlow 图，跑 `lyflow run`，与基线 results.csv 比对。

  python lyflow_ab.py --dataset <dataset.yml> --baseline <baseline_dir> [--lyflow lyflow.exe]
  python lyflow_ab.py --dataset <...> --baseline <...\\lyflow-gap-baseline-model> --model v12s0.onnx

不带 --model 比的是配置/模板路径，带 --model 比的是现场在用的模型 ROI 路径。

**历史对拍工具。** M7 起 gap 包的行为已有意偏离基线（ROI 只搬中心、fit_line 的 toward、
固定半径全路径生效、flush 默认带符号等），出现不一致是预期的；它不再是验收门槛，
只用来看偏离落在哪些样本上、偏了多少。

比三件事：
  1. 成功/失败状态一致
  2. 成功样本的 gap、flush 与基线 |Δ| ≤ 0.002 mm
  3. 同一张图里 gap.measure_reference 与拆分算子之差 ≤ 0.002 mm

另外**只报不判**地对一次现场值（数据目录的 manifest.csv，两位小数）：那是另一套口径，
容差 0.006 mm，用来回答「这条路径是不是现场跑的那条」。

前三条全部通过退出 0，否则 1。
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lyflow_graph_from_config as gen  # noqa: E402

TOLERANCE_MM = 0.002
# 现场值只有两位小数，而且和离线跑的是不同的一次构建，容差自然要松一档（§10）。
FIELD_TOLERANCE_MM = 0.006

# 拆分算子里读值的节点。判定节点在它们下游，值一样。
SPLIT_NODES = {"flush": "n_flush", "gap": "n_gap"}
REFERENCE_NODE = "n_ref"


class Args:
    """gen.build 要的那几个字段。argparse 的 Namespace 装不下额外键，索性自己给一个。"""

    def __init__(self, **kw):
        self.point_dir = None
        self.primary = None
        self.secondary = None
        self.template_dir = None
        self.sample_id = None
        self.graph_id = None
        self.name = None
        self.model = None
        self.__dict__.update(kw)


def read_field_values(manifest: Path, samples: list[dict]) -> dict[str, dict[str, float]]:
    """现场值：manifest.csv 的 value 列，按 **left_cloud 路径** join 到样本。
    按 (序列号, 测点, 维度) join 会把同一测点相隔 50 秒的两次测量（R4_16/R4_17）
    撞在一起 —— 那正是计划 §7 记的那一条，用路径就没有这个歧义。"""
    if not manifest.is_file():
        return {}
    by_cloud: dict[str, list[dict]] = {}
    with manifest.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            by_cloud.setdefault(row["left_cloud"].replace("\\", "/").lower(), []).append(row)
    out: dict[str, dict[str, float]] = {}
    for sample in samples:
        key = str((sample.get("pcd") or {}).get("primary", "")).replace("\\", "/").lower()
        for row in by_cloud.get(key, []):
            value = to_float(row.get("value", ""))
            if value is None:
                continue
            out.setdefault(sample["sample_id"], {})[
                "gap" if row["dimension"] == "Gap" else "flush"] = value
    return out


def default_lyflow() -> Path | None:
    """取**最新**的那一个 —— release 里常常留着一份不带算子包的旧产物，
    按固定顺序挑会让整份 A/B 静默地全跑成「算子不存在」。"""
    here = Path(__file__).resolve()
    # packs/gap/tools/ -> 仓库根
    found = []
    for repo in (here.parents[3], Path("D:/project/LyFlow")):
        for profile in ("release", "debug"):
            exe = repo / "bridge" / "target" / profile / "lyflow.exe"
            if exe.is_file():
                found.append(exe)
    if not found:
        return None
    return max(found, key=lambda p: p.stat().st_mtime)


def read_baseline(path: Path) -> dict[str, dict]:
    with (path / "results.csv").open(encoding="utf-8-sig", newline="") as f:
        return {row["sample_id"]: row for row in csv.DictReader(f)}


def to_float(text: str) -> float | None:
    text = (text or "").strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def run_graph(lyflow: Path, graph: Path) -> tuple[dict, list[str]]:
    """跑一张图，返回 {nodeId: {port: value}} 与节点级错误列表。"""
    proc = subprocess.run([str(lyflow), "run", str(graph)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    values: dict[str, dict] = {}
    errors: list[str] = []
    if not proc.stdout.strip():
        errors.append(f"lyflow run 没有任何事件（exit {proc.returncode}）: "
                      + " ".join(proc.stderr.split())[:300])
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("kind") != "node_state":
            continue
        node = event.get("nodeId", "")
        if event.get("state") == "error":
            errors.append(f"{node}: {(event.get('error') or {}).get('message', '')}")
        for out in ((event.get("stats") or {}).get("outputs") or []):
            values.setdefault(node, {})[out["port"]] = out.get("value")
    return values, errors


def measurement(values: dict, node: str, port: str) -> float | None:
    """节点没跑、或者测量没成功，都返回 None。"""
    value = (values.get(node) or {}).get(port)
    if not isinstance(value, dict) or not value.get("ok"):
        return None
    number = value.get("value")
    if number is None or not math.isfinite(number):
        return None
    return float(number)


def delta(a: float | None, b: float | None) -> float | None:
    if a is None or b is None:
        return None
    return abs(a - b)


def format_value(v: float | None) -> str:
    return "—" if v is None else f"{v:.4f}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True,
                        help="放 results.csv 的目录")
    parser.add_argument("--lyflow", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=None, help="生成的图放哪，默认临时目录")
    parser.add_argument("--only", action="append", help="只跑这些 sample_id（可重复）")
    parser.add_argument("--tolerance", type=float, default=TOLERANCE_MM)
    parser.add_argument("--json", type=Path, help="把逐样本结果也写一份 JSON")
    parser.add_argument("--model", type=Path, default=None,
                        help="走模型 ROI 路径：ONNX 模型文件（基线要换成 lyflow-gap-baseline-model）")
    parser.add_argument("--field-values", type=Path, default=None,
                        help="现场值 CSV，默认 <dataset 同目录>/manifest.csv")
    parser.add_argument("--field-tolerance", type=float, default=FIELD_TOLERANCE_MM)
    args = parser.parse_args(argv)

    lyflow = args.lyflow or default_lyflow()
    if not lyflow or not Path(lyflow).is_file():
        raise SystemExit("找不到 lyflow.exe，用 --lyflow 指一个")
    dataset = args.dataset.resolve()
    root = dataset.parent
    baseline = read_baseline(args.baseline.resolve())
    out_dir = (args.out or (Path(__import__("tempfile").gettempdir()) / "lyflow-gap-ab")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"lyflow: {lyflow}", file=sys.stderr)
    print(f"图放在: {out_dir}", file=sys.stderr)

    with dataset.open(encoding="utf-8") as f:
        samples = (yaml.safe_load(f) or {}).get("samples") or []
    if args.only:
        wanted = set(args.only)
        samples = [s for s in samples if s["sample_id"] in wanted]

    model = str(args.model.resolve()) if args.model else None
    if model:
        print(f"模型: {model}", file=sys.stderr)
    field = read_field_values(args.field_values or (root / "manifest.csv"), samples)

    rows = []
    for sample in samples:
        sample_id = sample["sample_id"]
        config = (root / sample["config"]).resolve()
        pcd = sample.get("pcd") or {}
        graph_path = out_dir / f"{gen.safe_id(sample_id)}.lyflow.json"
        build_args = Args(primary=str((root / pcd["primary"]).resolve()),
                          secondary=str((root / pcd["secondary"]).resolve()),
                          sample_id=sample_id,
                          graph_id=f"ab_{gen.safe_id(sample_id)}",
                          name=sample_id,
                          model=model)
        doc = gen.build(config, build_args)
        with graph_path.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")

        values, errors = run_graph(lyflow, graph_path)
        base = baseline.get(sample_id, {})
        row = {
            "sample": sample_id,
            "baselineGap": to_float(base.get("gap_mm", "")),
            "baselineFlush": to_float(base.get("flush_mm", "")),
            "baselineSuccess": (base.get("success", "") or "").lower() == "true",
            "refGap": measurement(values, REFERENCE_NODE, "gap"),
            "refFlush": measurement(values, REFERENCE_NODE, "flush"),
            "splitGap": measurement(values, SPLIT_NODES["gap"], "value"),
            "splitFlush": measurement(values, SPLIT_NODES["flush"], "value"),
            "errors": errors,
        }
        row["splitSuccess"] = row["splitGap"] is not None and row["splitFlush"] is not None
        row["dGap"] = delta(row["splitGap"], row["baselineGap"])
        row["dFlush"] = delta(row["splitFlush"], row["baselineFlush"])
        row["dRefGap"] = delta(row["splitGap"], row["refGap"])
        row["dRefFlush"] = delta(row["splitFlush"], row["refFlush"])
        # 现场值：只报不判 —— 两位小数、另一套构建，不是本脚本的通过判据
        row["fieldGap"] = (field.get(sample_id) or {}).get("gap")
        row["fieldFlush"] = (field.get(sample_id) or {}).get("flush")
        row["dFieldGap"] = delta(row["splitGap"], row["fieldGap"])
        row["dFieldFlush"] = delta(row["splitFlush"], row["fieldFlush"])

        problems = []
        if row["baselineSuccess"] != row["splitSuccess"]:
            problems.append("状态不一致")
        for key, name in (("Gap", "gap"), ("Flush", "flush")):
            b = row[f"baseline{key}"]
            s = row[f"split{key}"]
            if (b is None) != (s is None):
                problems.append(f"{name} 一边有值一边没有")
            elif b is not None and abs(b - s) > args.tolerance:
                problems.append(f"{name} Δ={abs(b - s):.5f}")
            d = row[f"dRef{key}"]
            r = row[f"ref{key}"]
            if (r is None) != (s is None):
                problems.append(f"{name} 与黑盒对照一边有值一边没有")
            elif d is not None and d > args.tolerance:
                problems.append(f"{name} 与黑盒对照 Δ={d:.5f}")
        row["problems"] = problems
        rows.append(row)
        print(f"  {sample_id}: {'ok' if not problems else '；'.join(problems)}", file=sys.stderr)

    # ------------------------------------------------------------------ 表格
    header = ("sample", "基线 gap", "基线 flush", "LyFlow gap", "LyFlow flush",
              "Δgap", "Δflush", "Δ现场 gap", "Δ现场 flush", "状态一致")
    widths = [max(len(header[0]), *(len(r["sample"]) for r in rows))] + [11] * 6 + [10, 12, 8]
    print("| " + " | ".join(h.ljust(w) for h, w in zip(header, widths)) + " |")
    print("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    fmt5 = lambda v: "—" if v is None else f"{v:.5f}"  # noqa: E731
    for r in rows:
        cells = [
            r["sample"],
            format_value(r["baselineGap"]),
            format_value(r["baselineFlush"]),
            format_value(r["splitGap"]),
            format_value(r["splitFlush"]),
            fmt5(r["dGap"]),
            fmt5(r["dFlush"]),
            fmt5(r["dFieldGap"]),
            fmt5(r["dFieldFlush"]),
            "是" if r["baselineSuccess"] == r["splitSuccess"] else "否",
        ]
        print("| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |")

    consistent = [r for r in rows if not r["problems"]]
    deltas = [d for r in rows for d in (r["dGap"], r["dFlush"]) if d is not None]
    print()
    print(f"一致 {len(consistent)} / {len(rows)}；"
          f"最大 |Δ| = {max(deltas):.6f} mm" if deltas else "没有可比的数值")

    # ------------------------------------------------------- 现场值（只报不判）
    field_deltas = [(r["sample"], name, d)
                    for r in rows
                    for name, d in (("gap", r["dFieldGap"]), ("flush", r["dFieldFlush"]))
                    if d is not None]
    if field_deltas:
        within = [x for x in field_deltas if x[2] <= args.field_tolerance]
        worst = max(field_deltas, key=lambda x: x[2])
        print(f"现场值（manifest.csv，容差 {args.field_tolerance} mm）："
              f"{len(within)} / {len(field_deltas)} 个数值一致；"
              f"最大 |Δ| = {worst[2]:.5f} mm（{worst[0]} {worst[1]}）")
        for sample, name, d in field_deltas:
            if d > args.field_tolerance:
                print(f"  超出：{sample} {name} Δ={d:.5f}")

    bad = [r for r in rows if r["problems"]]
    if bad:
        print("\n不一致的样本：")
        for r in bad:
            print(f"  {r['sample']}: {'；'.join(r['problems'])}")
            for e in r["errors"]:
                print(f"      {e}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        with args.json.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(rows, f, ensure_ascii=False, indent=2)
            f.write("\n")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
