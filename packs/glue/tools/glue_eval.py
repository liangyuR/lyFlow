"""真实帧的批量评估（glue-plan §4 第 14 / 15 / 18 条）：lyflow eval 跑 Glue1、Glue2 全部帧（示例图的参数），
逐帧列出找没找到胶路、报了几处断胶（位置）、胶宽、边距、判定，与五个积木的耗时。

    $env:LYFLOW_GLUE_DATA = "<演示数据解压目录>"
    python packs\\glue\\tools\\glue_eval.py [--out <仓库外的目录>] [--lyflow <带 glue 包的 lyflow.exe>]

门槛：第 14 条 满胶段（Glue1 #15–80、Glue2 #13–66）Glue1 0 帧报断胶、Glue2 ≤ 1 帧，两组都 100% 找到胶路；
第 15 条 无胶帧（两组 #1–10）都判 NG、理由是「检测区内没找到胶」；第 18 条 五个积木单帧 ≤ 50 ms。
报告写到 --out（report.md / report.json / 每组的 eval 原始输出）；任一条没过退出码 1。
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys

from glue_common import (DATASETS, NO_BEAD, data_dir, find_lyflow, fmt, frame_no, frames, md_table,
                         out_dir, output_value, run_eval, write_samples)

BLOCKS = ["n_path", "n_width", "n_breaks", "n_edge", "n_judge"]
METRICS = ["outputs.ok", "outputs.pathInfo.ok", "outputs.breakCount", "outputs.longestBreak",
           "outputs.coverage", "run.durationMs"] + [f"nodes.{b}.durationMs" for b in BLOCKS]


def evaluate(lyflow, data, out, name):
    cfg = DATASETS[name]
    fs = frames(data, name)
    samples = out / f"{name}.samples.jsonl"
    write_samples(samples, [{"id": f"{name}#{frame_no(p)}", "set": {"n_load.path": str(p)}} for p in fs])
    rows = run_eval(lyflow, cfg["graph"], samples, METRICS, sets={"n_load.source": "file"},
                    log=out / f"{name}.eval.jsonl", csv=out / f"{name}.eval.csv")
    result = []
    for row in rows:
        m = row["metrics"]
        breaks = output_value(row, "breaks") or {}
        verdict = output_value(row, "verdict") or {}
        info = output_value(row, "pathInfo") or {}
        result.append({
            "frame": int(row["sample"].split("#")[1]),
            "status": row["status"],
            "pathOk": bool(m.get("outputs.pathInfo.ok")),
            "heading": info.get("heading"),
            "coverage": m.get("outputs.coverage"),
            "breakCount": int(m.get("outputs.breakCount") or 0),
            "breaks": [[b.get("sStart"), b.get("sEnd"), b.get("length")] for b in breaks.get("breaks", [])],
            "widthMean": output_value(row, "widthMean"),
            "widthMin": output_value(row, "widthMin"),
            "widthMax": output_value(row, "widthMax"),
            "distanceMean": output_value(row, "distanceMean"),
            "ok": m.get("outputs.ok") == 1.0,
            "verdict": verdict.get("message"),
            "reason": info.get("reason"),
            "blocksMs": sum(m.get(f"nodes.{b}.durationMs") or 0.0 for b in BLOCKS),
            "runMs": m.get("run.durationMs"),
        })
    result.sort(key=lambda r: r["frame"])
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data")
    ap.add_argument("--lyflow")
    ap.add_argument("--out")
    ap.add_argument("--budget-ms", type=float, default=50.0)
    args = ap.parse_args()
    lyflow = find_lyflow(args.lyflow)
    data = data_dir(args.data)
    out = out_dir(args.out, "eval")
    print(f"lyflow: {lyflow}\n数据: {data}\n输出: {out}")

    report = {"lyflow": str(lyflow), "data": str(data), "sets": {}, "checks": []}
    md = ["# glue 真实帧评估", "", f"`{lyflow}`，数据 `{data}`。", ""]
    all_ok = True
    times = []
    for name, cfg in DATASETS.items():
        rows = evaluate(lyflow, data, out, name)
        report["sets"][name] = rows
        lo, hi = cfg["full"]
        full = [r for r in rows if lo <= r["frame"] <= hi]
        empty = [r for r in rows if cfg["empty"][0] <= r["frame"] <= cfg["empty"][1]]
        fp = [r for r in full if r["breakCount"] > 0]
        lost = [r for r in full if not r["pathOk"]]
        limit = 0 if name == "Glue1" else 1
        ok14 = len(fp) <= limit and not lost and len(full) == hi - lo + 1
        wrong15 = [r for r in empty if r["ok"] or r["verdict"] != NO_BEAD]
        ok15 = not wrong15 and len(empty) == cfg["empty"][1] - cfg["empty"][0] + 1
        report["checks"].append({"item": 14, "set": name, "ok": ok14, "frames": len(full),
                                 "falseBreakFrames": [(r["frame"], r["breaks"]) for r in fp],
                                 "pathLost": [r["frame"] for r in lost]})
        report["checks"].append({"item": 15, "set": name, "ok": ok15, "frames": len(empty),
                                 "wrong": [(r["frame"], r["verdict"]) for r in wrong15]})
        all_ok = all_ok and ok14 and ok15
        times += [r["blocksMs"] for r in rows if r["status"] == "ok"]

        md += [f"## {name}（{cfg['form']}，{len(rows)} 帧）", ""]
        md.append(f"- 第 14 条（满胶段 #{lo}–{hi}，{len(full)} 帧）：报断胶 **{len(fp)}** 帧（门槛 ≤ {limit}），"
                  f"没找到胶路 **{len(lost)}** 帧 → {'通过' if ok14 else '未通过'}")
        for r in fp:
            md.append(f"  - #{r['frame']}：断口 {r['breaks']}")
        md.append(f"- 第 15 条（无胶帧 #{cfg['empty'][0]}–{cfg['empty'][1]}，{len(empty)} 帧）：判 NG 且理由是"
                  f"「{NO_BEAD}」的 **{len(empty) - len(wrong15)}** 帧 → {'通过' if ok15 else '未通过'}")
        for r in wrong15:
            md.append(f"  - #{r['frame']}：{r['verdict']}")
        md.append("")
        md.append(md_table(
            ["帧", "胶路", "方向°", "覆盖率", "断胶", "断口 [sStart, sEnd, 长]", "胶宽 均/小/大", "边距", "判定", "五积木 ms"],
            [[r["frame"], "✓" if r["pathOk"] else "✗", fmt(r["heading"], 0), fmt(r["coverage"], 2), r["breakCount"],
              "; ".join(f"[{b[0]:.0f}, {b[1]:.0f}, {b[2]:.0f}]" for b in r["breaks"]),
              f"{fmt(r['widthMean'])} / {fmt(r['widthMin'])} / {fmt(r['widthMax'])}", fmt(r["distanceMean"]),
              r["verdict"], fmt(r["blocksMs"])] for r in rows]))
        md.append("")

    times.sort()
    p50 = statistics.median(times)
    p95 = times[int(0.95 * (len(times) - 1))]
    ok18 = times[-1] <= args.budget_ms
    report["checks"].append({"item": 18, "ok": ok18, "p50": p50, "p95": p95, "max": times[-1], "n": len(times)})
    all_ok = all_ok and ok18
    md += ["## 第 18 条：五个积木（bead_path → judge）单帧耗时", "",
           f"{len(times)} 帧：中位 **{p50:.1f} ms**、p95 {p95:.1f} ms、最大 **{times[-1]:.1f} ms**"
           f"（门槛 ≤ {args.budget_ms:.0f} ms）→ {'通过' if ok18 else '未通过'}。"
           "来源：eval 每行的 nodes.<积木>.durationMs 相加。", ""]

    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "report.md").write_text("\n".join(md), encoding="utf-8")
    for c in report["checks"]:
        print(json.dumps(c, ensure_ascii=False))
    print(f"报告：{out / 'report.md'}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
