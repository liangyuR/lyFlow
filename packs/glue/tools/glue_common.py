"""packs/glue/tools 的共用件：找 lyflow CLI、找数据、跑 lyflow eval、读回每一行（D14：只调 CLI，不自己做检测）。

数据目录由 LYFLOW_GLUE_DATA（或 --data）给，下面是 Glue1/、Glue2/ 两个子目录，帧叫 FrameN_1.jpg。
真实帧与它们的叠画都是客户数据（D12）：这里产出的一切都写到仓库外（--out，默认 %TEMP%\\lyflow-glue-*）。
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GRAPHS = ROOT / "packs" / "glue" / "graphs"

# 两个演示数据集：图、目录、满胶段与无胶段（glue-plan §4 第 14 / 15 条）
DATASETS = {
    "Glue1": {"graph": GRAPHS / "glue1.lyflow.json", "form": "straight", "full": (15, 80), "empty": (1, 10)},
    "Glue2": {"graph": GRAPHS / "glue2.lyflow.json", "form": "swirl", "full": (13, 66), "empty": (1, 10)},
}

NO_BEAD = "检测区内没找到胶"


def die(message: str) -> None:
    print(message, file=sys.stderr)
    sys.exit(2)


def find_lyflow(explicit: str | None = None) -> Path:
    """带 glue 包的 lyflow.exe：--lyflow / LYFLOW_EXE，否则 bridge/target/{release,debug} 里最新的那个。"""
    candidates = []
    if explicit or os.environ.get("LYFLOW_EXE"):
        candidates = [Path(explicit or os.environ["LYFLOW_EXE"])]
    else:
        for profile in ("release", "debug"):
            exe = ROOT / "bridge" / "target" / profile / "lyflow.exe"
            if exe.exists():
                candidates.append(exe)
        candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    for exe in candidates:
        if not exe.exists():
            continue
        r = subprocess.run([str(exe), "manifest"], capture_output=True, text=True, encoding="utf-8")
        if r.returncode == 0 and '"glue.bead_path"' in r.stdout:
            return exe
    die(
        "找不到带 glue 包的 lyflow.exe。先构建一份：\n"
        '  $env:LYFLOW_PACKS = "glue"; cargo build --release --manifest-path bridge/Cargo.toml '
        "--bin lyflow --no-default-features\n"
        "或者用 --lyflow / LYFLOW_EXE 指一个。"
    )
    raise AssertionError  # die 不返回


def data_dir(explicit: str | None = None) -> Path:
    raw = explicit or os.environ.get("LYFLOW_GLUE_DATA")
    if not raw:
        die("设 LYFLOW_GLUE_DATA（或 --data）指到演示数据的解压目录（下面有 Glue1/、Glue2/）")
    d = Path(raw)
    if not (d / "Glue1").is_dir() or not (d / "Glue2").is_dir():
        die(f"{d} 下面没有 Glue1/ 与 Glue2/")
    return d


def out_dir(explicit: str | None, name: str) -> Path:
    d = Path(explicit) if explicit else Path(tempfile.gettempdir()) / f"lyflow-glue-{name}"
    d.mkdir(parents=True, exist_ok=True)
    inside = ROOT.resolve()
    try:
        d.resolve().relative_to(inside)
        die(f"--out {d} 在仓库里面：真实帧与它们的叠画是客户数据（D12），写到仓库外")
    except ValueError:
        pass
    return d


def frames(data: Path, dataset: str) -> list[Path]:
    """FrameN_1.jpg，按 N 排序（_2 / _3 与 _1 字节相同，只用 _1）。"""
    out = []
    for p in (data / dataset).glob("Frame*_1.jpg"):
        m = re.fullmatch(r"Frame(\d+)_1\.jpg", p.name)
        if m:
            out.append((int(m.group(1)), p))
    return [p for _, p in sorted(out)]


def frame_no(path: str | Path) -> int:
    m = re.search(r"Frame(\d+)_1", Path(path).name)
    return int(m.group(1)) if m else -1


def write_samples(path: Path, rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def run_eval(lyflow: Path, graph: Path, samples: Path, metrics: list[str], sets: dict | None = None,
             log: Path | None = None, csv: Path | None = None) -> list[dict]:
    """lyflow eval <graph> --samples <jsonl> --metric … --summary，返回每个样本的 eval_row（带 summary）。

    指标只放每一帧都取得到值的（没找到胶时胶宽、边距是 null，eval 在第一帧取不到值就退出 4）；
    胶宽、边距、断口的位置从 summary 里的图级输出读。"""
    cmd = [str(lyflow), "eval", str(graph), "--samples", str(samples), "--summary"]
    for k, v in (sets or {}).items():
        cmd += ["--set", f"{k}={json.dumps(v, ensure_ascii=False)}"]
    for m in metrics:
        cmd += ["--metric", m]
    if csv:
        cmd += ["--csv", str(csv)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if log:
        log.write_text(r.stdout, encoding="utf-8")
        log.with_suffix(".err.txt").write_text(r.stderr, encoding="utf-8")
    if r.returncode not in (0, 1):
        die(f"lyflow eval 失败（退出码 {r.returncode}）：\n{r.stderr[-2000:]}")
    rows = []
    for line in r.stdout.splitlines():
        if not line.startswith("{"):
            continue
        j = json.loads(line)
        if j.get("kind") == "eval_row":
            rows.append(j)
    return rows


def output_value(row: dict, name: str):
    """eval_row.summary.outputs.<name>.value：Measurement 给 value（null → None），Record 给 data。"""
    entry = ((row.get("summary") or {}).get("outputs") or {}).get(name) or {}
    v = entry.get("value")
    if not isinstance(v, dict):
        return None
    if v.get("kind") == "Record":
        return v.get("data")
    if v.get("kind") == "Measurement":
        return v.get("value")
    return v


def md_table(header: list[str], rows: list[list]) -> str:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for r in rows:
        out.append("| " + " | ".join("" if c is None else str(c) for c in r) + " |")
    return "\n".join(out)


def fmt(v, digits: int = 1) -> str:
    return "—" if v is None else f"{v:.{digits}f}"
