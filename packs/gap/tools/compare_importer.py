#!/usr/bin/env python
"""逐节点等价：C++ 导入器（lyflow import）与 Python 生成器产出的图对比。

  python compare_importer.py [--dataset <dataset.yml>] [--lyflow lyflow.exe] [--model v12s0.onnx]

比节点集合（id + op + 参数值）与边集合（from/to 的节点与端口），
忽略 ui 坐标、meta、图的 id / name。

**历史对拍工具。** M7 起 C++ 导入器按新的算子参数生成图（fit_line 接 toward、
business_rois 写 datumSide、顶层参数 gapOffset / modelPath），Python 生成器没有跟着改，
两边不再逐节点相同；它不再是验收门槛。

路径参数两边形态不同是预期的（Python 写绝对路径，导入器写相对 baseDir 的相对路径），
比较时统一规范化成绝对路径再比。

数据集里的配置目录没有 setting.yml，所以模型/回退两种形态用一份**暂存夹具**造出来：
把 StandardGap.yml 复制进 <work>/<测点>_<tag>/<测点>/，并在它的父目录放一份只有
model_roi 段的 setting.yml。auto 的推导规则因此与 Python 生成器完全无关地被单独测到。

六种组合：
  template          kind=:template          vs Python 不带 --model —— 逐节点
  auto(no setting)  kind=auto，原目录        必须退回 template，且逐节点等于 Python
  auto(off)         kind=auto，enabled=false 必须退回 template
  model(off)        kind=:model，enabled=false vs Python 带 --model —— 逐节点
  auto(on)          kind=auto，enabled=true  带 flow.fallback 的完整图，结构断言
  model(on)         kind=:model，enabled=true 同上

退出码 0 = 全部一致。
"""
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lyflow_graph_from_config as gen  # noqa: E402

REL_TOL = 1e-9
DEFAULT_DATASET = r"C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904\dataset.yml"
DEFAULT_MODEL = r"C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx"
PATH_PARAMS = {"dir", "configPath", "templateDir", "modelPath", "primaryFile", "secondaryFile"}
FALLBACK_NODES = ["n_fb_crop_p", "n_fb_crop_s", "n_fb_flushBase", "n_fb_flushRef",
                  "n_fb_gapLeft", "n_fb_gapRight", "n_fb_line", "n_fb_merged", "n_fb_overall",
                  "n_fb_quality_base", "n_fb_ref_point"]
FALLBACK_NODES_WITH_REF = sorted(FALLBACK_NODES + ["n_fb_quality_ref"])
BACKUP_FLUSH_NODES = ["b_n_crop_flushBase", "b_n_crop_flushRef", "b_n_fit_base"]


class Args:
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


def default_lyflow() -> Path | None:
    here = Path(__file__).resolve()
    found = []
    for profile in ("release", "debug"):
        exe = here.parents[3] / "bridge" / "target" / profile / "lyflow.exe"
        if exe.is_file():
            found.append(exe)
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def run_import(lyflow: Path, config: Path, kind: str) -> tuple[dict | None, str]:
    proc = subprocess.run(
        [str(lyflow), "import", str(config), "--kind", kind, "--base-dir", str(config.parent)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    text = proc.stdout.strip()
    if not text.startswith("{"):
        return None, f"exit {proc.returncode}: {text[:300]} {proc.stderr.strip()[:300]}"
    doc = json.loads(text)
    if "nodes" not in doc:
        return None, f"导入器返回诊断: {text[:300]}"
    return doc, ""


def norm_path(value, base: Path) -> str:
    p = Path(str(value))
    if not p.is_absolute():
        p = base / p
    try:
        return str(p.resolve()).lower()
    except OSError:
        return str(p).lower()


def norm_params(node: dict, base: Path) -> dict:
    return {k: (norm_path(v, base) if k in PATH_PARAMS else v)
            for k, v in (node.get("params") or {}).items()}


def same_value(a, b) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b or a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if math.isnan(a) or math.isnan(b):
            return math.isnan(a) and math.isnan(b)
        return math.isclose(a, b, rel_tol=REL_TOL, abs_tol=1e-12)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same_value(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same_value(a[k], b[k]) for k in a)
    return a == b


def edge_set(doc: dict) -> set:
    return {(e["from"]["node"], e["from"]["port"], e["to"]["node"], e["to"]["port"])
            for e in doc["edges"]}


def compare(py: dict, cpp: dict, base: Path) -> list[str]:
    problems = []
    py_nodes = {n["id"]: n for n in py["nodes"]}
    cpp_nodes = {n["id"]: n for n in cpp["nodes"]}
    for missing in sorted(set(py_nodes) - set(cpp_nodes)):
        problems.append(f"导入器少了节点 {missing}")
    for extra in sorted(set(cpp_nodes) - set(py_nodes)):
        problems.append(f"导入器多了节点 {extra}")
    for node_id in sorted(set(py_nodes) & set(cpp_nodes)):
        a, b = py_nodes[node_id], cpp_nodes[node_id]
        if a["op"] != b["op"]:
            problems.append(f"{node_id} op 不同: {a['op']} vs {b['op']}")
        pa, pb = norm_params(a, base), norm_params(b, base)
        for key in sorted(set(pa) | set(pb)):
            if key not in pa:
                problems.append(f"{node_id}.{key} 只有导入器有: {pb[key]!r}")
            elif key not in pb:
                problems.append(f"{node_id}.{key} 只有 Python 有: {pa[key]!r}")
            elif not same_value(pa[key], pb[key]):
                problems.append(f"{node_id}.{key} 不同: {pa[key]!r} vs {pb[key]!r}")
    for e in sorted(edge_set(py) - edge_set(cpp)):
        problems.append(f"导入器少了边 {e[0]}.{e[1]} -> {e[2]}.{e[3]}")
    for e in sorted(edge_set(cpp) - edge_set(py)):
        problems.append(f"导入器多了边 {e[0]}.{e[1]} -> {e[2]}.{e[3]}")
    oa = {k: (v["node"], v["port"]) for k, v in (py.get("outputs") or {}).items()}
    ob = {k: (v["node"], v["port"]) for k, v in (cpp.get("outputs") or {}).items()}
    if oa != ob:
        problems.append(f"outputs 不同: {oa} vs {ob}")
    return problems


def check_fallback(doc: dict) -> list[str]:
    problems = []
    nodes = {n["id"]: n for n in doc["nodes"]}
    fbs = sorted(i for i, n in nodes.items() if n["op"] == "flow.fallback")
    expected = FALLBACK_NODES_WITH_REF if "n_fit_ref" in nodes else FALLBACK_NODES
    if fbs != expected:
        problems.append(f"fallback 节点集合不对: {fbs}")
    if not any(i.startswith("b_") for i in nodes):
        problems.append("没有 b_ 前缀的模板备用闭包")
    for need in BACKUP_FLUSH_NODES:
        if need not in nodes:
            problems.append(f"备用闭包缺了 {need}")
    for node_id, node in nodes.items():
        if node["op"] != "gap.fit_line":
            continue
        want = "roi_intersection" if node_id.startswith("b_") else "inlier_ends"
        got = (node.get("params") or {}).get("endpoints")
        if got != want:
            problems.append(f"{node_id}.endpoints 是 {got!r}，该是 {want!r}")
    edges = edge_set(doc)
    for src, _, dst, port in edges:
        if src.startswith("b_") and not dst.startswith("b_"):
            if nodes[dst]["op"] != "flow.fallback" or port != "b":
                problems.append(f"备用闭包 {src} 经非惰性端口流出到 {dst}.{port}")
    for fb in fbs:
        ports = {port for _, _, dst, port in edges if dst == fb}
        if ports != {"a", "b"}:
            problems.append(f"{fb} 的输入端口不是 a+b: {sorted(ports)}")
    if {"gap", "flush", "bundle"} != set((doc.get("outputs") or {})):
        problems.append(f"outputs 不是 gap/flush/bundle: {sorted(doc.get('outputs') or {})}")
    return problems


def stage(config: Path, work: Path, tag: str, enabled: bool, model: str) -> Path:
    root = work / f"{config.parent.name}_{tag}"
    point = root / config.parent.name
    point.mkdir(parents=True, exist_ok=True)
    target = point / config.name
    target.write_text(config.read_text(encoding="utf-8"), encoding="utf-8", newline="")
    flag = "true" if enabled else "false"
    (root / "setting.yml").write_text(
        "model_roi:\n  enabled: " + flag + "\n  model_path: " + model + "\n",
        encoding="utf-8", newline="")
    return target


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", type=Path, default=Path(DEFAULT_DATASET))
    parser.add_argument("--lyflow", type=Path, default=None)
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help="写进 setting.yml 的 ONNX 路径。图里只存路径，文件在不在都能比")
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)

    lyflow = args.lyflow or default_lyflow()
    if not lyflow or not Path(lyflow).is_file():
        raise SystemExit("找不到 lyflow.exe，用 --lyflow 指一个")
    dataset = args.dataset.resolve()
    root = dataset.parent
    with dataset.open(encoding="utf-8") as f:
        samples = (yaml.safe_load(f) or {}).get("samples") or []
    configs = [(s["sample_id"], (root / s["config"]).resolve()) for s in samples]
    print(f"lyflow: {lyflow}", file=sys.stderr)
    print(f"{len(configs)} 个样本，{len({c for _, c in configs})} 份不同的配置", file=sys.stderr)

    work = Path(args.out) if args.out else Path(tempfile.gettempdir()) / "lyflow-import-cmp"
    work.mkdir(parents=True, exist_ok=True)

    state = {"failures": 0}
    counted = {"template": 0, "auto(no setting)": 0, "auto(off)": 0, "model(off)": 0,
               "auto(on)": 0, "model(on)": 0}

    def report(label: str, problems: list[str]) -> None:
        if not problems:
            return
        state["failures"] += 1
        print(f"  {label}: {len(problems)} 处不同")
        for p in problems[:12]:
            print(f"      {p}")

    def imported(label: str, config: Path, kind: str) -> dict | None:
        doc, why = run_import(lyflow, config, kind)
        if doc is None:
            state["failures"] += 1
            print(f"  {label}: 导入失败 {why}")
        return doc

    def is_model_graph(doc: dict) -> bool:
        return any(n["op"] == "gap.roi_from_labels" for n in doc["nodes"])

    for sample_id, config in configs:
        base = config.parent
        cpp = imported(f"{sample_id} template", config, "StandardGap.yml:template")
        if cpp is not None:
            counted["template"] += 1
            report(f"{sample_id} template",
                   compare(gen.build(config, Args(point_dir=str(base))), cpp, base))

        cpp = imported(f"{sample_id} auto(no setting)", config, "StandardGap.yml")
        if cpp is not None:
            counted["auto(no setting)"] += 1
            if is_model_graph(cpp):
                report(f"{sample_id} auto(no setting)", ["没有 setting.yml 却走了模型路径"])
            else:
                report(f"{sample_id} auto(no setting)",
                       compare(gen.build(config, Args(point_dir=str(base))), cpp, base))

        staged = stage(config, work, "off", False, args.model)
        cpp = imported(f"{sample_id} auto(off)", staged, "StandardGap.yml")
        if cpp is not None:
            counted["auto(off)"] += 1
            if is_model_graph(cpp):
                report(f"{sample_id} auto(off)", ["model_roi.enabled=false 却走了模型路径"])
        cpp = imported(f"{sample_id} model(off)", staged, "StandardGap.yml:model")
        if cpp is not None:
            if any(n["op"] == "flow.fallback" for n in cpp["nodes"]):
                report(f"{sample_id} model(off)", ["enabled=false 时不该带 flow.fallback"])
            else:
                counted["model(off)"] += 1
                py = gen.build(staged, Args(point_dir=str(staged.parent), model=args.model))
                report(f"{sample_id} model(off)", compare(py, cpp, staged.parent))

        staged = stage(config, work, "on", True, args.model)
        for kind, key in (("StandardGap.yml", "auto(on)"), ("StandardGap.yml:model", "model(on)")):
            cpp = imported(f"{sample_id} {key}", staged, kind)
            if cpp is None:
                continue
            counted[key] += 1
            problems = [] if is_model_graph(cpp) else ["enabled=true 却没走模型路径"]
            problems += check_fallback(cpp)
            infer = [n for n in cpp["nodes"] if n["op"] == "ml.onnx_run"]
            if not infer or infer[0].get("params", {}).get("modelPath") != args.model:
                problems.append("modelPath 不是 setting.yml 里那一个")
            report(f"{sample_id} {key}", problems)

    print()
    print("组合计数：" + "，".join(f"{k} {v}" for k, v in counted.items()))
    print("全部一致" if not state["failures"] else f"{state['failures']} 处不一致")
    return 0 if not state["failures"] else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
