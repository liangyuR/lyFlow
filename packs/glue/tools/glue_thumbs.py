"""叠画缩略图（glue-plan §4 第 17 条）：Glue1、Glue2 全部帧，每帧画上判定积木的叠画（胶路、两条胶边、零件边点、
缺陷、结论），另拼成几张总览图，逐张过。

    $env:LYFLOW_GLUE_DATA = "<演示数据解压目录>"
    python packs\\glue\\tools\\glue_thumbs.py [--out <仓库外的目录>] [--lyflow <exe>] [--cols 5] [--thumb 384]

叠画是 lyflow eval 跑示例图时图级输出 overlay（glue.judge 的叠画）原样画上去的（D14：检测只在 lyflow 里）；
配色与编辑器同一张 role 表（glue_draw.py）。真实帧与它们的叠画都是客户数据（D12），只写到仓库外。
"""

from __future__ import annotations

import argparse

import cv2

from glue_common import DATASETS, data_dir, find_lyflow, fmt, frame_no, frames, out_dir, output_value, run_eval, \
    write_samples
from glue_draw import caption, contact_sheet, draw, read_gray_as_bgr, write_image


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data")
    ap.add_argument("--lyflow")
    ap.add_argument("--out")
    ap.add_argument("--cols", type=int, default=5)
    ap.add_argument("--rows", type=int, default=4)
    ap.add_argument("--thumb", type=int, default=384, help="缩略图宽度（px）")
    args = ap.parse_args()
    lyflow = find_lyflow(args.lyflow)
    data = data_dir(args.data)
    out = out_dir(args.out, "thumbs")
    print(f"lyflow: {lyflow}\n数据: {data}\n输出: {out}")

    for name, cfg in DATASETS.items():
        fs = frames(data, name)
        by_id = {f"{name}#{frame_no(p)}": p for p in fs}
        samples = out / f"{name}.samples.jsonl"
        write_samples(samples, [{"id": k, "set": {"n_load.path": str(p)}} for k, p in by_id.items()])
        rows = run_eval(lyflow, cfg["graph"], samples, ["outputs.ok"], sets={"n_load.source": "file"},
                        log=out / f"{name}.eval.jsonl")
        (out / name).mkdir(exist_ok=True)
        tiles = []
        for row in sorted(rows, key=lambda r: int(r["sample"].split("#")[1])):
            k = int(row["sample"].split("#")[1])
            img = read_gray_as_bgr(by_id[row["sample"]])
            overlay = output_value(row, "overlay") or {}
            verdict = (output_value(row, "verdict") or {}).get("message", row["status"])
            ok = output_value(row, "ok") == 1.0
            text = (f"{name} #{k}  {verdict}  宽 {fmt(output_value(row, 'widthMean'))}"
                    f"  边距 {fmt(output_value(row, 'distanceMean'))} px")
            full = caption(draw(img, overlay.get("items", []), font=0.6), text,
                           color=(153, 211, 52) if ok else (113, 113, 248), size=18)
            write_image(out / name / f"Frame{k}.jpg", full)
            scale = args.thumb / img.shape[1]
            small = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
            thumb = caption(draw(small, [it for it in overlay.get("items", []) if it.get("kind") != "text"],
                                 scale=scale), f"#{k} {verdict}",
                            color=(153, 211, 52) if ok else (113, 113, 248), size=13)
            tiles.append(thumb)
        per = args.cols * args.rows
        for i in range(0, len(tiles), per):
            sheet = contact_sheet(tiles[i:i + per], args.cols)
            first = int(sorted(rows, key=lambda r: int(r["sample"].split("#")[1]))[i]["sample"].split("#")[1])
            write_image(out / f"{name}_sheet{i // per + 1}_from{first}.jpg", sheet)
        print(f"{name}: {len(tiles)} 帧 → {out / name}，总览 {(len(tiles) + per - 1) // per} 张")


if __name__ == "__main__":
    main()
