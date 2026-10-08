"""人造断胶数据集（glue-plan §4 第 16 条）：在满胶帧上用 glue.synth_break 盖掉一段胶，另一条检测链去查。

    $env:LYFLOW_GLUE_DATA = "<演示数据解压目录>"
    python packs\\glue\\tools\\glue_synth.py [--out <仓库外的目录>] [--lyflow <exe>] [--seed 20260925]

做法（全在 lyflow 里，脚本只拼图、写样本、跑 lyflow eval、读结果 —— D14）：
- 由示例图拼一张「两条链」的图：读图 → bead_path → bead_width → synth_break → bead_path → bead_width → bead_breaks，
  两条链的参数都照示例图；另挂一个 bead_breaks 看原帧本身有没有断胶（有就不要这个样本）。
- 满胶段的每一帧（Glue1 #15–80、Glue2 #13–66）× 长度 {15, 30, 60} px，断口起点在检测区里随机（两头各让 24 px）。
- synth_break 两侧都试，挑源带更干净的一侧；两侧都不干净（或源带跨过零件边）就不做，这个样本记「跳过」。

门槛：直胶 ≥ 30 px 检出 100%、起止误差 ≤ 5 px；螺旋胶 ≥ 30 px 检出 ≥ 90%、60 px 检出 100%、起止误差 ≤ 32 px；
15 px（短于 minLength）不报；断口之外零误报；每类有效样本 ≥ 20。报告写到 --out；没过退出码 1。
"""

from __future__ import annotations

import argparse
import copy
import json
import random
import sys

from glue_common import (DATASETS, data_dir, find_lyflow, frame_no, frames, md_table, out_dir,
                         output_value, run_eval, write_samples)

LENGTHS = [15, 30, 60]
GATES = {  # class -> length -> (最低检出率, 起止误差上限)
    "straight": {30: (1.0, 5.0), 60: (1.0, 5.0)},
    "swirl": {30: (0.9, 32.0), 60: (1.0, 32.0)},
}
MIN_SAMPLES = 20


def synth_graph(base: dict) -> dict:
    """示例图 → 两条链的图。第二条链的节点带 2 后缀，参数照抄第一条。"""
    nodes = {n["id"]: n for n in base["nodes"]}
    out = {"schemaVersion": 1, "id": "01JGLUESYNTHBREAK000000000", "name": "人造断胶 · " + base.get("name", ""),
           "nodes": [], "edges": []}
    keep = ["n_load", "n_path", "n_width", "n_breaks"]
    for nid in keep:
        out["nodes"].append(copy.deepcopy(nodes[nid]))
    out["nodes"].append({"id": "n_synth", "op": "glue.synth_break", "params": {}})
    for nid in ["n_path", "n_width", "n_breaks"]:
        n2 = copy.deepcopy(nodes[nid])
        n2["id"] = nid + "2"
        out["nodes"].append(n2)
    edges = [("n_load", "image", "n_path", "image"), ("n_load", "image", "n_width", "image"),
             ("n_path", "path", "n_width", "path"), ("n_width", "bead", "n_breaks", "bead"),
             ("n_load", "image", "n_synth", "image"), ("n_width", "bead", "n_synth", "bead"),
             ("n_synth", "image", "n_path2", "image"), ("n_synth", "image", "n_width2", "image"),
             ("n_path2", "path", "n_width2", "path"), ("n_width2", "bead", "n_breaks2", "bead")]
    out["edges"] = [{"id": f"e{i}", "from": {"node": a, "port": b}, "to": {"node": c, "port": d}}
                    for i, (a, b, c, d) in enumerate(edges)]
    out["outputs"] = {
        "synth": {"node": "n_synth", "port": "info"},
        "baseBreaks": {"node": "n_breaks", "port": "count"},
        "breaks": {"node": "n_breaks2", "port": "breaks"},
        "breakCount": {"node": "n_breaks2", "port": "count"},
        "pathInfo": {"node": "n_path2", "port": "path.info"},
        "line": {"node": "n_path2", "port": "path.line"},
        # 叠画：没过的样本画出来看（--dump-failures）
        "ovSynth": {"node": "n_synth", "port": "overlay"},
        "ovPath": {"node": "n_path2", "port": "overlay"},
        "ovWidth": {"node": "n_width2", "port": "overlay"},
        "ovBreaks": {"node": "n_breaks2", "port": "overlay"},
    }
    return out


def dump_failure(lyflow, graph, out, row_sample, sets, overlays, title):
    """没过的样本：lyflow dump 出人造断胶的那一帧，把第二条链的叠画画上去。"""
    import subprocess

    import struct
    import cv2
    import numpy as np
    from glue_draw import caption, draw, write_image

    safe = row_sample.replace("#", "_").replace("/", "_")
    png = out / "failures" / f"{safe}.lyim"
    png.parent.mkdir(exist_ok=True)
    cmd = [str(lyflow), "dump", str(graph), "n_synth:image", str(png)]
    for k, v in sets.items():
        cmd += ["--set", f"{k}={json.dumps(v, ensure_ascii=False)}"]
    subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if not png.exists():
        return
    payload = png.read_bytes()
    if len(payload) < 48:
        raise ValueError("LYIM 头不完整")
    magic, w, h, channels, depth, level, fw, fh, row, rows, row_bytes, _ = struct.unpack_from("<12I", payload)
    if (magic, channels, depth, level, row, rows, row_bytes) != (0x4D49594C, 1, 1, 0, 0, h, w):
        raise ValueError("人造断胶应输出完整的 u8 灰度 LYIM")
    mono = np.frombuffer(payload, dtype=np.uint8, count=w*h, offset=48).reshape(h, w)
    img = cv2.cvtColor(mono, cv2.COLOR_GRAY2BGR)
    items = []
    for ov in overlays:
        items += (ov or {}).get("items", [])
    write_image(out / "failures" / f"{safe}.jpg", caption(draw(img, items), title))


def overlaps(b, lo, hi):
    return min(b[1], hi) - max(b[0], lo) > 0


def project(line: dict, p) -> float | None:
    """图上一点投到胶路折线上，返回那一处的 s（两条检测链的 s 不是同一把尺子，比较前先换到同一条胶路上）。"""
    if not line or not p:
        return None
    pts, ss = line["points"], line["s"]
    best, best_s = None, None
    for i in range(len(pts) - 1):
        ax, ay = pts[i]
        bx, by = pts[i + 1]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / L2))
        qx, qy = ax + t * dx, ay + t * dy
        d = (qx - p[0]) ** 2 + (qy - p[1]) ** 2
        if best is None or d < best:
            best, best_s = d, ss[i] + t * (ss[i + 1] - ss[i])
    return best_s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data")
    ap.add_argument("--lyflow")
    ap.add_argument("--out")
    ap.add_argument("--seed", type=int, default=20260925)
    ap.add_argument("--dump-failures", action="store_true", help="没过的样本 dump 出图、画上叠画（写到 --out/failures）")
    args = ap.parse_args()
    lyflow = find_lyflow(args.lyflow)
    data = data_dir(args.data)
    out = out_dir(args.out, "synth")
    rnd = random.Random(args.seed)
    print(f"lyflow: {lyflow}\n数据: {data}\n输出: {out}")

    report = {"lyflow": str(lyflow), "seed": args.seed, "classes": {}, "checks": []}
    md = ["# glue 人造断胶", "", f"`{lyflow}`，数据 `{data}`，随机种子 {args.seed}。", ""]
    all_ok = True
    for name, cfg in DATASETS.items():
        base = json.loads(cfg["graph"].read_text(encoding="utf-8"))
        graph = out / f"synth_{name}.lyflow.json"
        graph.write_text(json.dumps(synth_graph(base), ensure_ascii=False, indent=1), encoding="utf-8")
        zone = next(n for n in base["nodes"] if n["id"] == "n_path")["params"]["zone"]
        lo, hi = cfg["full"]
        samples = []
        for p in frames(data, name):
            k = frame_no(p)
            if not lo <= k <= hi:
                continue
            for L in LENGTHS:
                s0 = round(rnd.uniform(zone[0] + 24, zone[1] - L - 24), 1)
                samples.append({"id": f"{name}#{k}/L{L}", "set": {"n_load.path": str(p), "n_synth.sStart": s0,
                                                                  "n_synth.length": L},
                                "tags": {"L": str(L)}})
        sp = out / f"synth_{name}.samples.jsonl"
        write_samples(sp, samples)
        rows = run_eval(lyflow, graph, sp, ["outputs.breakCount", "outputs.baseBreaks", "outputs.synth.ok"],
                        sets={"n_load.source": "file"}, log=out / f"synth_{name}.eval.jsonl")
        form = cfg["form"]
        results = []
        for row in rows:
            frame, lpart = row["sample"].split("#")[1].split("/")
            L = int(lpart[1:])
            synth = output_value(row, "synth") or {}
            base_breaks = row["metrics"].get("outputs.baseBreaks")
            path_ok = bool((output_value(row, "pathInfo") or {}).get("ok"))
            got = [[b["sStart"], b["sEnd"], b["length"]] for b in (output_value(row, "breaks") or {}).get("breaks", [])]
            r = {"frame": int(frame), "L": L, "status": row["status"], "sStart": synth.get("sStart"),
                 "sEnd": synth.get("sEnd"), "side": synth.get("side"), "breaks": got, "pathOk": path_ok}
            if row["status"] != "ok":
                r["skip"] = "运行失败：" + ",".join(row.get("errors", []))
            elif base_breaks:
                r["skip"] = "原帧本身报了断胶"
            elif not synth.get("ok"):
                r["skip"] = synth.get("reason") or "没做成"
            else:
                # 真值：原帧胶路上断口两端的那两个点，投到这一帧自己的胶路上（投不上就用原帧的 s）
                line = output_value(row, "line")
                a = project(line, synth.get("start")) or synth["sStart"]
                b = project(line, synth.get("end")) or synth["sEnd"]
                r["truthOnPath"] = [round(a, 1), round(b, 1)]
                hit = [x for x in got if overlaps(x, a, b)]
                r["fp"] = [x for x in got if not overlaps(x, a, b)]
                r["hit"] = bool(hit)
                if hit:
                    best = max(hit, key=lambda x: min(x[1], b) - max(x[0], a))
                    r["errStart"] = round(best[0] - a, 1)
                    r["errEnd"] = round(best[1] - b, 1)
            results.append(r)
            gate = GATES[form].get(L)
            bad = "skip" not in r and ((L < 20 and r["hit"]) or (L >= 20 and not r["hit"]) or r["fp"] or
                                       ("errStart" in r and gate and max(abs(r["errStart"]), abs(r["errEnd"])) > gate[1]))
            if bad and args.dump_failures:
                sets = {"n_load.source": "file", **next(x["set"] for x in samples if x["id"] == row["sample"])}
                dump_failure(lyflow, graph, out, row["sample"], sets,
                             [output_value(row, k) for k in ("ovPath", "ovWidth", "ovBreaks", "ovSynth")],
                             f"{row['sample']}  truth [{r['sStart']:.0f}, {r['sEnd']:.0f}]  got {r['breaks']}")
        report["classes"][form] = results

        md += [f"## {name}（{form}）", ""]
        table = []
        for L in LENGTHS:
            rs = [r for r in results if r["L"] == L and "skip" not in r]
            skipped = [r for r in results if r["L"] == L and "skip" in r]
            hits = [r for r in rs if r["hit"]]
            errs = [abs(r[k]) for r in hits for k in ("errStart", "errEnd")]
            fp = sum(len(r["fp"]) for r in rs)
            rate = len(hits) / len(rs) if rs else 0.0
            maxerr = max(errs) if errs else None
            if L in GATES[form]:
                need, tol = GATES[form][L]
                ok = len(rs) >= MIN_SAMPLES and rate >= need - 1e-9 and (maxerr is None or maxerr <= tol) and fp == 0
                gate = f"检出 ≥ {need:.0%}、误差 ≤ {tol:.0f} px、零误报"
            else:
                ok = len(rs) >= MIN_SAMPLES and not hits and fp == 0
                gate = "不报、零误报"
            all_ok = all_ok and ok
            report["checks"].append({"item": 16, "form": form, "L": L, "n": len(rs), "skipped": len(skipped),
                                     "detected": len(hits), "rate": rate, "maxErr": maxerr, "falsePositives": fp,
                                     "ok": ok})
            table.append([f"{L} px", len(rs), len(skipped), f"{len(hits)}（{rate:.0%}）",
                          "—" if maxerr is None else f"{maxerr:.1f}", fp, gate, "通过" if ok else "未通过"])
        md.append(md_table(["长度", "有效样本", "跳过", "检出", "起止误差最大 px", "误报", "门槛", "结果"], table))
        md += ["", "逐样本（断口真值是原帧胶路的 s，误差 = 报出的 − 真值）：", ""]
        md.append(md_table(
            ["帧", "长度", "真值 [sStart, sEnd]", "源带", "报出", "起 / 止误差", "误报", "备注"],
            [[f"#{r['frame']}", r["L"], "—" if r.get("sStart") is None else f"[{r['sStart']:.1f}, {r['sEnd']:.1f}]",
              r.get("side") or "—", "; ".join(f"[{x[0]:.0f}, {x[1]:.0f}]" for x in r["breaks"]),
              "" if "errStart" not in r else f"{r['errStart']:+.1f} / {r['errEnd']:+.1f}",
              len(r.get("fp", [])), r.get("skip") or ("漏检" if r.get("hit") is False and r["L"] >= 20 else
                                                     ("报了短断口" if r.get("hit") and r["L"] < 20 else ""))]
             for r in results]))
        md.append("")

    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "report.md").write_text("\n".join(md), encoding="utf-8")
    for c in report["checks"]:
        print(json.dumps(c, ensure_ascii=False))
    print(f"报告：{out / 'report.md'}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
