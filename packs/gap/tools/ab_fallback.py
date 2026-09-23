#!/usr/bin/env python
"""回退图的 A/B：用 `lyflow import` 产出带 flow.fallback 的完整图，跑它，与基线比 gap/flush。

  python ab_fallback.py --dataset <dataset.yml> --baseline "%TEMP%\\lyflow-gap-baseline-model"
  python ab_fallback.py --dataset <...> --baseline "%TEMP%\\lyflow-gap-baseline" \\
      --break-model --only <sample_id> --only <sample_id>

与 `lyflow_ab.py` 的区别：图不是 Python 生成器产的，而是 C++ 导入器产的**回退图**
（模型 ROI 主路径 + `b_` 前缀的模板备用闭包，见 packs/gap/README.md）。

**历史对拍工具。** M7 起 gap 包的行为已有意偏离基线（ROI 四角点变换、fit_line 的 toward、
固定半径全路径生效、flush 默认带符号等），出现不一致是预期的；它不再是验收门槛，
只用来看偏离落在哪些样本上、偏了多少。

`lyflow import` 只拿 (文本, baseDir)，所以每份配置先暂存一份夹具：
`<out>/stage/<key>/` 里放 StandardGap.yml、模板目录，以及一份
`model_roi.enabled: true` 的 setting.yml。点云不进夹具 —— 两个 load 节点
（`n_load` 与 `b_n_load`）都用 `--set` 覆盖成 `source=files` + 两个绝对路径。

`--break-model` 把 `n_infer.modelPath` 指到一个不存在的文件，主路径必失败，
用来实测回退分支的数值：那时基线要换成**模板基线**。

全部一致退出 0，否则 1。
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lyflow_ab as ab  # noqa: E402
import lyflow_graph_from_config as gen  # noqa: E402

TOLERANCE_MM = 0.002
DEFAULT_MODEL = r"C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx"
CHOICE_NODE = "n_fb_flushBase"
MISSING_MODEL = "no-such-model.onnx"


def stage(config: Path, work: Path, model: str) -> Path:
    key = gen.safe_id("_".join(config.resolve().parts[-3:-1]))
    point = work / "stage" / key
    point.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(config, point / config.name)
    templates = config.parent / config.stem
    if templates.is_dir():
        shutil.copytree(templates, point / config.stem, dirs_exist_ok=True)
    (point / "setting.yml").write_text(
        "model_roi:\n  enabled: true\n  model_path: " + model + "\n",
        encoding="utf-8", newline="")
    return point / config.name


def import_graph(lyflow: Path, config: Path, out: Path) -> tuple[dict | None, str]:
    out.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        [str(lyflow), "import", str(config), "--kind", "StandardGap.yml:model",
         "--base-dir", str(config.parent), "-o", str(out)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not out.is_file():
        return None, f"exit {proc.returncode}: {proc.stderr.strip()[:300]}"
    doc = json.loads(out.read_text(encoding="utf-8"))
    if not any(n["op"] == "flow.fallback" for n in doc["nodes"]):
        return None, "导入器没有产出带 flow.fallback 的图"
    return doc, ""


def run_graph(lyflow: Path, graph: Path, base: Path, primary: Path, secondary: Path,
              broken: str | None) -> tuple[dict, list[str], bool, str]:
    args = [str(lyflow), "run", str(graph), "--base-dir", str(base)]
    for node in ("n_load", "b_n_load"):
        args += ["--set", f"{node}.source=" + json.dumps("files"),
                 "--set", f"{node}.primaryFile=" + json.dumps(str(primary)),
                 "--set", f"{node}.secondaryFile=" + json.dumps(str(secondary))]
    if broken:
        args += ["--set", "n_infer.modelPath=" + json.dumps(broken)]
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    values: dict[str, dict] = {}
    errors: list[str] = []
    extended = False
    choice = ""
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
        if event.get("kind") == "plan_extended":
            extended = True
            continue
        if event.get("kind") != "node_state":
            continue
        node = event.get("nodeId", "")
        if event.get("state") == "error":
            errors.append(f"{node}: {(event.get('error') or {}).get('message', '')}")
        for out in ((event.get("stats") or {}).get("outputs") or []):
            values.setdefault(node, {})[out["port"]] = out.get("value")
            if node == CHOICE_NODE and out["port"] == "choice":
                choice = ((out.get("value") or {}).get("data") or {}).get("choice", "")
    return values, errors, extended, choice


def baseline_fallbacks(baseline: Path) -> set[str]:
    path = baseline / "diagnostics.jsonl"
    if not path.is_file():
        return set()
    hit = set()
    for line in path.open(encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        reason = row.get("fallback_reason")
        if reason not in (None, "", "None"):
            hit.add(row.get("sample_id"))
    return hit


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--lyflow", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--only", action="append")
    parser.add_argument("--tolerance", type=float, default=TOLERANCE_MM)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--break-model", action="store_true",
                        help="把 n_infer.modelPath 指到不存在的文件，逼出回退分支")
    args = parser.parse_args(argv)

    lyflow = args.lyflow or ab.default_lyflow()
    if not lyflow or not Path(lyflow).is_file():
        raise SystemExit("找不到 lyflow.exe，用 --lyflow 指一个")
    dataset = args.dataset.resolve()
    root = dataset.parent
    baseline = ab.read_baseline(args.baseline.resolve())
    work = (args.out or (Path(tempfile.gettempdir()) / "lyflow-gap-ab-fallback")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    broken = str(work / MISSING_MODEL) if args.break_model else None
    print(f"lyflow: {lyflow}", file=sys.stderr)
    print(f"夹具与图放在: {work}", file=sys.stderr)
    if broken:
        print(f"人为失败: n_infer.modelPath = {broken}", file=sys.stderr)

    with dataset.open(encoding="utf-8") as f:
        samples = (yaml.safe_load(f) or {}).get("samples") or []
    if args.only:
        wanted = set(args.only)
        samples = [s for s in samples if s["sample_id"] in wanted]

    graphs: dict[str, tuple[Path, Path]] = {}
    rows = []
    for sample in samples:
        sample_id = sample["sample_id"]
        config = (root / sample["config"]).resolve()
        if str(config) not in graphs:
            staged = stage(config, work, args.model)
            path = work / "graphs" / (gen.safe_id(staged.parent.name) + ".lyflow.json")
            doc, why = import_graph(lyflow, staged, path)
            if doc is None:
                print(f"  {sample_id}: 导入失败 {why}", file=sys.stderr)
                rows.append({"sample": sample_id, "problems": [f"导入失败 {why}"], "errors": []})
                continue
            graphs[str(config)] = (path, staged.parent)
            print(f"  图 {path.name}: {len(doc['nodes'])} 节点, {len(doc['edges'])} 连线",
                  file=sys.stderr)
        graph, base = graphs[str(config)]
        pcd = sample.get("pcd") or {}
        values, errors, extended, choice = run_graph(
            lyflow, graph, base, (root / pcd["primary"]).resolve(),
            (root / pcd["secondary"]).resolve(), broken)

        base_row = baseline.get(sample_id, {})
        row = {
            "sample": sample_id,
            "choice": choice,
            "planExtended": extended,
            "fellBack": choice == "b" or extended,
            "baselineGap": ab.to_float(base_row.get("gap_mm", "")),
            "baselineFlush": ab.to_float(base_row.get("flush_mm", "")),
            "baselineSuccess": (base_row.get("success", "") or "").lower() == "true",
            "gap": ab.measurement(values, "n_gap", "value"),
            "flush": ab.measurement(values, "n_flush", "value"),
            "errors": errors,
        }
        row["success"] = row["gap"] is not None and row["flush"] is not None
        row["dGap"] = ab.delta(row["gap"], row["baselineGap"])
        row["dFlush"] = ab.delta(row["flush"], row["baselineFlush"])

        problems = []
        if row["baselineSuccess"] != row["success"]:
            problems.append("状态不一致")
        for key, name in (("Gap", "gap"), ("Flush", "flush")):
            b = row[f"baseline{key}"]
            s = row[key.lower()]
            if (b is None) != (s is None):
                problems.append(f"{name} 一边有值一边没有")
            elif b is not None and abs(b - s) > args.tolerance:
                problems.append(f"{name} Δ={abs(b - s):.5f}")
        if broken and not row["fellBack"]:
            problems.append("人为失败了却没有回退")
        row["problems"] = problems
        rows.append(row)
        mark = "b" if row["fellBack"] else "a"
        print(f"  {sample_id}[{mark}]: {'ok' if not problems else '；'.join(problems)}",
              file=sys.stderr)

    header = ("sample", "选路", "基线 gap", "基线 flush", "回退图 gap", "回退图 flush",
              "Δgap", "Δflush")
    widths = [max(len(header[0]), *(len(r["sample"]) for r in rows))] + [6] + [11] * 4 + [10, 10]
    print("| " + " | ".join(h.ljust(w) for h, w in zip(header, widths)) + " |")
    print("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    for r in rows:
        cells = [r["sample"], r.get("choice") or "—",
                 ab.format_value(r.get("baselineGap")), ab.format_value(r.get("baselineFlush")),
                 ab.format_value(r.get("gap")), ab.format_value(r.get("flush")),
                 "—" if r.get("dGap") is None else f"{r['dGap']:.5f}",
                 "—" if r.get("dFlush") is None else f"{r['dFlush']:.5f}"]
        print("| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |")

    good = [r for r in rows if not r["problems"]]
    deltas = [d for r in rows for d in (r.get("dGap"), r.get("dFlush")) if d is not None]
    print()
    print(f"一致 {len(good)} / {len(rows)}；"
          + (f"最大 |Δ| = {max(deltas):.6f} mm" if deltas else "没有可比的数值"))

    fired = {r["sample"] for r in rows if r.get("fellBack")}
    expected = baseline_fallbacks(args.baseline.resolve()) & {r["sample"] for r in rows}
    print(f"回退触发的样本 {len(fired)} 个: {sorted(fired) if fired else '（空）'}")
    print(f"基线 fallback_reason 非空的样本 {len(expected)} 个: "
          f"{sorted(expected) if expected else '（空）'}")
    print("两个集合" + ("相等" if fired == expected else f"不等，差集 {sorted(fired ^ expected)}"))

    bad = [r for r in rows if r["problems"]]
    if bad:
        print("\n不一致的样本：")
        for r in bad:
            print(f"  {r['sample']}: {'；'.join(r['problems'])}")
            for e in r["errors"][:6]:
                print(f"      {e}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        with args.json.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(rows, f, ensure_ascii=False, indent=2)
            f.write("\n")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
