"""彩色胶条算子的真实帧评估（docs/glue-measure-ops.md「1.3 彩色测量算子」）：只出报告，不判门禁。

    $env:LYFLOW_GLUE_COLOR_DATA = "<采集目录>"   # 下面是 cam1/、cam2/…（camN_s000001.png，已去马赛克的 RGB）与 teach.json
    python packs\\glue\\tools\\glue_color_eval.py [--teach <teach.json>] [--out <仓库外的目录>] [--lyflow <lyflow.exe>]

teach.json 手写，每一条是一段示教：

    {"entries": [{"name": "cam1 直胶", "cam": "cam1", "teach": 35, "points": [[600, 125], [20, 664]],
                  "frames": [1, 60], "beadWidth": 30, "draw": [35, 50],
                  "model": {…color_model 参数…}, "runs": {…color_runs 参数…}}]}

modelFrom = 前面某一条的 name：不另建模，用那一条的模型（例如绕圈胶丝用同一相机直胶段示教的模型）。

每条：示教帧 + 示教线 → glue.color_model；frames 范围里的每一帧 → glue.color_map（模型 JSON 填进参数）→ 沿示教线
glue.color_runs，另跑 glue.color_fan → glue.color_track（锚点 = 示教线第一个点，扇区 = 示教方向 ± 30°）。
逐帧列出：中心有段的站占比、中心落在无信息区间的站占比（高光 / 暗区）、有信息却没有段的站占比、中心段宽度、
跟踪的覆盖率，与两个打分节点的耗时。真实帧是现场数据：报告、叠画都写到仓库外（--out，默认 %TEMP%\\lyflow-glue-color）。
"""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import subprocess
from pathlib import Path

from glue_common import die, find_lyflow, fmt, md_table, out_dir, output_value, run_eval, write_samples


def frame_path(data: Path, cam: str, n: int) -> Path:
    return data / cam / f"{cam}_s{n:06d}.png"


def node(nid: str, op: str, params: dict) -> dict:
    return {"id": nid, "op": op, "params": params}


def graph(nodes: list[dict], edges: list[tuple[str, str]], outputs: dict[str, str]) -> dict:
    es = []
    for i, (a, b) in enumerate(edges):
        (fa, pa), (fb, pb) = a.split("."), b.split(".")
        es.append({"id": f"e{i}", "from": {"node": fa, "port": pa}, "to": {"node": fb, "port": pb}})
    outs = {}
    for k, v in outputs.items():
        n, p = v.split(".")
        outs[k] = {"node": n, "port": p}
    return {"schemaVersion": 1, "id": "01JGLUECOLOREVAL000000000", "nodes": nodes, "edges": es,
            "outputs": outs}


def heading_deg(points) -> float:
    (x0, y0), (x1, y1) = points[0], points[1]
    return math.degrees(math.atan2(y1 - y0, x1 - x0))


def line_length(points) -> float:
    return sum(math.dist(points[i], points[i + 1]) for i in range(len(points) - 1))


def teach_model(lyflow: Path, data: Path, e: dict, out: Path) -> dict:
    g = graph([node("n_load", "io.load_image", {"source": "file", "path": str(frame_path(data, e["cam"], e["teach"]))}),
               node("n_line", "glue.polyline", {"points": json.dumps(e["points"])}),
               node("n_model", "glue.color_model", e.get("model", {}))],
              [("n_load.image", "n_model.image"), ("n_line.line", "n_model.line")],
              {"model": "n_model.model"})
    path = out / f"{e['slug']}.teach.lyflow.json"
    path.write_text(json.dumps(g, ensure_ascii=False, indent=2), encoding="utf-8")
    r = subprocess.run([str(lyflow), "run", str(path), "--summary"], capture_output=True, text=True,
                       encoding="utf-8")
    (out / f"{e['slug']}.teach.jsonl").write_text(r.stdout, encoding="utf-8")
    for line in r.stdout.splitlines():
        if line.startswith("{") and json.loads(line).get("kind") == "run_summary":
            model = output_value({"summary": json.loads(line)}, "model")
            if model:
                return model
    die(f"{e['name']}：建模没跑出来（退出码 {r.returncode}）\n{r.stderr[-2000:]}")
    raise AssertionError


def production_graph(e: dict, model: dict) -> dict:
    pts = e["points"]
    W = e.get("beadWidth", 30)
    runs = {"stationStep": 4, "searchHalf": max(40, W + 20)} | e.get("runs", {})
    reach = runs["searchHalf"] + 8
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    roi = [min(xs) - reach, min(ys) - reach, max(xs) - min(xs) + 2 * reach, max(ys) - min(ys) + 2 * reach]
    h = heading_deg(pts)
    anchor = pts[0]
    L = line_length(pts)
    return graph(
        [node("n_load", "io.load_image", {"source": "file"}),
         node("n_line", "glue.polyline", {"points": json.dumps(pts)}),
         node("n_map", "glue.color_map", {"model": json.dumps(model), "roi": roi}),
         node("n_runs", "glue.color_runs", runs),
         node("n_fan", "glue.color_fan", {"anchor": anchor, "sector": [h - 30, h + 30], "radial": [20, min(160, L)]}),
         node("n_track", "glue.color_track", {"anchor": anchor, "zone": [10, L], "bearingRange": [h - 30, h + 30],
                                              "widthMax": 2 * W, "searchHalf": max(40, W + 20)})],
        [("n_load.image", "n_map.image"), ("n_map.maps", "n_runs.maps"), ("n_line.line", "n_runs.line"),
         ("n_map.maps", "n_fan.maps"), ("n_map.maps", "n_track.maps"), ("n_fan.scan", "n_track.fan")],
        {"runs": "n_runs.runs", "track": "n_track.track"})


def summarize(runs: dict, W: float) -> dict:
    max_mid = max(8.0, 0.5 * W)
    st = runs.get("stations", [])
    present = blind = missing = 0
    widths, info = [], []
    causes: dict[str, int] = {}
    for s in st:
        info.append(s.get("infoFraction") or 0.0)
        central = [r for r in s["runs"] if abs(r["mid"]) <= max_mid]
        if central:
            best = min(central, key=lambda r: abs(r["mid"]))
            present += 1
            widths.append(best["width"])
            continue
        cause = next((u["cause"] for u in s["unknownSpans"] if u["lo"] <= 0 <= u["hi"]), None)
        if cause:
            blind += 1
            causes[cause] = causes.get(cause, 0) + 1
        else:
            missing += 1
    n = max(len(st), 1)
    return {"stations": len(st), "present": present / n, "unknown": blind / n, "missing": missing / n,
            "causes": causes, "info": statistics.fmean(info) if info else None,
            "width": statistics.median(widths) if widths else None,
            "widthP10": sorted(widths)[len(widths) // 10] if widths else None,
            "widthP90": sorted(widths)[(len(widths) * 9) // 10] if widths else None}


def draw(data: Path, e: dict, frame: int, runs: dict, out: Path) -> None:
    try:
        import cv2
    except ImportError:
        return
    img = cv2.imread(str(frame_path(data, e["cam"], frame)))
    if img is None:
        return
    W = e.get("beadWidth", 30)
    for s in runs.get("stations", []):
        c, n = s["center"], s["normal"]
        at = lambda t: (int(round(c[0] + n[0] * t)), int(round(c[1] + n[1] * t)))
        for u in s["unknownSpans"]:
            cv2.line(img, at(u["lo"]), at(u["hi"]), (0, 220, 255), 1)
        central = [r for r in s["runs"] if abs(r["mid"]) <= max(8.0, 0.5 * W)]
        for r in s["runs"]:
            cv2.line(img, at(r["lo"]), at(r["hi"]), (0, 255, 0) if r in central else (255, 160, 0), 1)
        if not central:
            cv2.circle(img, at(0), 2, (0, 0, 255), -1)
    cv2.imwrite(str(out / f"{e['slug']}_{frame:03d}.jpg"), img)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data")
    ap.add_argument("--teach")
    ap.add_argument("--lyflow")
    ap.add_argument("--out")
    args = ap.parse_args()
    raw = args.data or os.environ.get("LYFLOW_GLUE_COLOR_DATA")
    if not raw:
        die("设 LYFLOW_GLUE_COLOR_DATA（或 --data）指到采集目录（下面是 cam1/、cam2/… 与 teach.json）")
    data = Path(raw)
    teach = Path(args.teach) if args.teach else data / "teach.json"
    if not teach.is_file():
        die(f"没有 {teach}：手写一份示教（格式见本文件开头）")
    lyflow = find_lyflow(args.lyflow)
    if '"glue.color_map"' not in subprocess.run([str(lyflow), "manifest"], capture_output=True, text=True,
                                                 encoding="utf-8").stdout:
        die(f"{lyflow} 里没有 1.3 的彩色算子，重新构建一份")
    out = out_dir(args.out, "color")
    entries = json.loads(teach.read_text(encoding="utf-8"))["entries"]
    print(f"lyflow: {lyflow}\n数据: {data}\n输出: {out}")

    report = {"lyflow": str(lyflow), "data": str(data), "entries": []}
    md = ["# 彩色胶条算子真实帧评估", "", f"`{lyflow}`，数据 `{data}`，示教 `{teach}`。", "",
          "列：中心有段 = 中心 ±max(8, W/2) 里有一段的站占比；无信息 = 中心落在高光 / 暗区里、没有段；"
          "缺段 = 有信息却没有段（宿主会当断胶看的那些站）；info = 卡尺上有信息的采样占比的均值；"
          "跟踪 = color_track 的 coverage / coverageInformative。", ""]
    models: dict[str, dict] = {}
    for i, e in enumerate(entries):
        e["slug"] = f"{i:02d}_{e['cam']}"
        if e.get("modelFrom"):
            if e["modelFrom"] not in models:
                die(f"{e['name']}：modelFrom {e['modelFrom']} 不在它前面")
            model = models[e["modelFrom"]]
        else:
            model = teach_model(lyflow, data, e, out)
        models[e["name"]] = model
        q = model.get("quality", {})
        W = e.get("beadWidth", 30)
        g = production_graph(e, model)
        gpath = out / f"{e['slug']}.lyflow.json"
        gpath.write_text(json.dumps(g, ensure_ascii=False, indent=2), encoding="utf-8")
        a, b = e.get("frames", [e["teach"], e["teach"]])
        fs = [n for n in range(a, b + 1) if frame_path(data, e["cam"], n).exists()]
        samples = out / f"{e['slug']}.samples.jsonl"
        write_samples(samples, [{"id": f"{e['cam']}#{n}", "set": {"n_load.path": str(frame_path(data, e["cam"], n))}}
                                for n in fs])
        rows = run_eval(lyflow, gpath, samples, ["run.durationMs", "nodes.n_map.durationMs", "nodes.n_runs.durationMs"],
                        log=out / f"{e['slug']}.eval.jsonl")
        result = []
        for row in rows:
            n = int(row["sample"].split("#")[1])
            runs = output_value(row, "runs") or {}
            track = output_value(row, "track") or {}
            s = summarize(runs, W)
            s |= {"frame": n, "status": row["status"],
                  "trackCoverage": track.get("coverage"), "trackInformative": track.get("coverageInformative"),
                  "unknownSteps": track.get("unknownSteps"),
                  "mapMs": row["metrics"].get("nodes.n_map.durationMs"),
                  "runsMs": row["metrics"].get("nodes.n_runs.durationMs")}
            result.append(s)
            if n == e["teach"] or n in e.get("draw", []):
                draw(data, e, n, runs, out)
        result.sort(key=lambda r: r["frame"])
        report["entries"].append({"entry": {k: v for k, v in e.items() if k != "slug"}, "quality": q,
                                  "frames": result})
        md += [f"## {e['name']}（{e['cam']}，示教帧 {e['teach']}，期望胶宽约 {W} px）", "",
               "模型：" + "，".join(f"{k} = {fmt(q.get(k), 3) if isinstance(q.get(k), float) else q.get(k)}"
                                  for k in ("usable", "separation", "components", "glueCount", "bgCount",
                                            "saturatedFraction", "darkFraction", "glueLikeFraction",
                                            "centerShift", "recentered")),
               "",
               md_table(["帧", "中心有段", "无信息", "缺段", "无信息原因", "info", "中心段宽 P10 / P50 / P90",
                         "跟踪", "map ms", "runs ms"],
                        [[r["frame"], fmt(r["present"], 2), fmt(r["unknown"], 2), fmt(r["missing"], 2),
                          " ".join(f"{k}:{v}" for k, v in r["causes"].items()), fmt(r["info"], 2),
                          f"{fmt(r['widthP10'])} / {fmt(r['width'])} / {fmt(r['widthP90'])}",
                          f"{fmt(r['trackCoverage'], 2)} / {fmt(r['trackInformative'], 2)}",
                          fmt(r["mapMs"]), fmt(r["runsMs"])] for r in result]),
               ""]
        times = [r["mapMs"] + r["runsMs"] for r in result if r["mapMs"] is not None and r["runsMs"] is not None]
        if times:
            md += [f"color_map + color_runs：p50 {fmt(statistics.median(times))} ms，最大 {fmt(max(times))} ms。", ""]
        print(f"{e['name']}：{len(result)} 帧")
    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    (out / "report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"报告：{out / 'report.md'}")


if __name__ == "__main__":
    main()
