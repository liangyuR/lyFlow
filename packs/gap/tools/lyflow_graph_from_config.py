#!/usr/bin/env python
"""从 StandardGap.yml 生成一张 LyFlow 图（G6）。

用法：
  python lyflow_graph_from_config.py <StandardGap.yml> <point_dir> -o graph.lyflow.json
  python lyflow_graph_from_config.py <StandardGap.yml> --primary a.pcd --secondary b.pcd -o g.json
  python lyflow_graph_from_config.py <StandardGap.yml> ... --model v12s0.onnx -o g.json

不带 --model 是配置/模板 + ICP 路径；带 --model 是现场在用的模型 ROI 路径
（无模板、无 ICP，多一个跟随零件的裁剪窗，H5）。
六十多个参数手填必错，所以图是生成的；生成之后照样能在 LyFlow 里改。
参数一律毫米（G4），与 YAML 里一模一样，不用心算。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import yaml

# LyFlow 的 localId 允许的字符（见 schema/graph-doc.schema.json）
SAFE = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"

# 这条路径上不支持的几何类型。计划 §6 明确不做。
# nearest point 不在这一行里：数据集里有 5 个样本用它，见 docs/gap-acceptance.md「偏离与决策」。
UNSUPPORTED_TYPES = {"2-points line", "circle tangent"}

# 裁剪一律走标准算子的开区间（原 gap.crop_box 的语义，已删）
CROP_OPEN = {"bounds": "open"}


def safe_id(text: str) -> str:
    return "".join(c if c in SAFE else "_" for c in text)


class GraphBuilder:
    def __init__(self) -> None:
        self.nodes: list[dict] = []
        self.edges: list[dict] = []
        self._columns: dict[str, int] = {}

    def node(self, node_id: str, op: str, params: dict | None = None,
             column: int = 0, row: int = 0, title: str | None = None) -> str:
        ui: dict = {"position": {"x": column * 280, "y": row * 130}}
        if title:
            ui["title"] = title
        entry = {"id": node_id, "op": op, "ui": ui}
        if params:
            entry["params"] = params
        self.nodes.append(entry)
        return node_id

    def edge(self, src: str, src_port: str, dst: str, dst_port: str) -> None:
        self.edges.append({
            "id": f"e{len(self.edges):03d}",
            "from": {"node": src, "port": src_port},
            "to": {"node": dst, "port": dst_port},
        })

    def doc(self, graph_id: str, name: str) -> dict:
        return {
            "schemaVersion": 1,
            "id": graph_id,
            "name": name,
            "meta": {"app": "lyflow_graph_from_config.py"},
            "nodes": self.nodes,
            "edges": self.edges,
        }


def roi_list(values) -> list[float]:
    if values is None:
        return [0.0, 0.0, 0.0, 0.0]
    out = [float(v) for v in values]
    if len(out) != 4:
        raise SystemExit(f"ROI 必须是四个数，拿到的是 {values!r}")
    return out


def candidate_rois(candidate: dict, flush: dict, gap: dict) -> dict[str, list[float]]:
    """候选自带 rois 就用它的，否则回落到配置里那一份（复刻 configure_business_rois）。"""
    rois = candidate.get("rois") or {}
    croi_flush = rois.get("flush") or {}
    croi_gap = rois.get("gap") or {}
    return {
        "roiFlushBase": roi_list(croi_flush.get("base_roi", flush.get("base_roi"))),
        "roiGapLeft": roi_list(croi_gap.get("left_roi", gap.get("left_roi"))),
        "roiFlushRef": roi_list(croi_flush.get("ref_roi", flush.get("ref_roi"))),
        "roiGapRight": roi_list(croi_gap.get("right_roi", gap.get("right_roi"))),
    }


def template_candidates(align: dict) -> list[dict]:
    listed = align.get("template_candidates")
    if listed:
        return list(listed)
    # 没写候选就是单模板，用 label 模式存下来的那两个文件名
    return [{"id": "primary", "left": "left_template.pcd", "right": "right_template.pcd"}]


def build(config_path: Path, args) -> dict:
    with config_path.open("r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    common = cfg.get("common_settings") or {}
    flush = cfg.get("flush") or {}
    gap = cfg.get("gap") or {}
    align = cfg.get("align") or {}
    icp = align.get("ICP") or {}

    if (common.get("seg_mode") or "ROI") != "ROI":
        raise SystemExit(f"只支持 seg_mode: ROI，这份配置是 {common.get('seg_mode')!r}")
    if not align.get("align_cloud", False):
        raise SystemExit("align_cloud 为 false 时是 label 模式，不产生测量结果")

    base_side = str(flush.get("base_side", "left"))
    base_type = str(flush.get("base_type", "fit line"))
    ref_type = str(flush.get("ref_type", "line end"))
    left_type = str(gap.get("left_type", "circle"))
    right_type = str(gap.get("right_type", "circle"))
    for name, value in (("flush.base_type", base_type), ("flush.ref_type", ref_type),
                        ("gap.left_type", left_type), ("gap.right_type", right_type)):
        if value in UNSUPPORTED_TYPES:
            raise SystemExit(f"{name} = {value!r} 不在这条路径的支持范围内（计划 §6）")
    if "line" not in base_type:
        raise SystemExit(f"flush.base_type 目前只支持直线类，拿到 {base_type!r}")
    if ref_type not in ("line end", "selected point", "nearest point"):
        raise SystemExit(
            f"flush.ref_type 目前只支持 line end / selected point / nearest point，拿到 {ref_type!r}")
    if left_type != "circle" or right_type != "circle":
        raise SystemExit("gap 两侧目前只支持 circle")

    template_dir = Path(args.template_dir) if args.template_dir else (
        config_path.parent / config_path.stem)

    g = GraphBuilder()

    # -- 读盘 ---------------------------------------------------------------
    load_params: dict = {}
    if args.primary and args.secondary:
        load_params["source"] = "files"
        load_params["primaryFile"] = str(Path(args.primary))
        load_params["secondaryFile"] = str(Path(args.secondary))
    elif args.point_dir:
        load_params["dir"] = str(Path(args.point_dir))
    else:
        raise SystemExit("要么给 point_dir，要么给 --primary/--secondary")
    if args.model:
        # 模型输入必须是原始 1280 槽（NaN 槽保留，§7）
        load_params["dropNonFinite"] = False
    load = g.node("n_load", "gap.load_profile_pair", load_params, 0, 1, "读一对剖面")

    filter_cfg = common.get("filter") or {}

    def radius_outlier(source, source_port, column, row):
        """合并云上的半径离群。配置里没开就原样返回上游。"""
        if not filter_cfg.get("using_removal", False):
            return source, source_port
        flt = g.node("n_filter", "filter.radius_outlier", {
            # LyFlow 内置算子用米，这里替用户换算（G4）
            "radius": float(filter_cfg.get("filter_radius", 0.0)) / 1000.0,
            "minNeighbors": int(filter_cfg.get("filter_neighbors", 0)),
        }, column, row, "半径离群")
        g.edge(source, source_port, flt, "cloud")
        return flt, "cloud"

    if args.model:
        # 模型 ROI 路径（§9）：短路，不做整体 ROI 裁剪、不做 ICP、不用模板（§7）。
        # ONNX 三步（T6）：通道构造与 argmax 是领域约定，中间那一步是通用推理算子。
        tensor = g.node("n_tensor", "gap.profile_tensor", None, 1, 4, "剖面张量")
        g.edge(load, "primary", tensor, "primary")
        g.edge(load, "secondary", tensor, "secondary")
        infer = g.node("n_infer", "ml.onnx_run", {"modelPath": str(Path(args.model))},
                       1, 5, "ONNX 推理")
        g.edge(tensor, "tensor", infer, "input")
        seg = g.node("n_seg", "gap.labels_from_logits", None, 1, 6, "逐槽 argmax")
        g.edge(infer, "output", seg, "tensor")

        frame_p = g.node("n_frame_p", "gap.to_measurement_frame", None, 1, 0, "换轴 primary")
        frame_s = g.node("n_frame_s", "gap.to_measurement_frame", None, 1, 2, "换轴 secondary")
        g.edge(load, "primary", frame_p, "cloud")
        g.edge(load, "secondary", frame_s, "cloud")

        # 着色剖面：接测量帧的云（换轴不删点，槽位仍与标签一一对应），
        # 排在 drop_non_finite 之前 —— 删过点的云槽位会错位。
        colored = g.node("n_labels_p", "gap.labels_to_cloud", {"row": "primary"}, 2, 7, "着色 primary")
        g.edge(frame_p, "cloud", colored, "cloud")
        g.edge(seg, "labels", colored, "labels")

        rois = g.node("n_rois", "gap.roi_from_labels", {"baseSide": base_side}, 3, 4, "模型四框")
        g.edge(load, "primary", rois, "primary")
        g.edge(load, "secondary", rois, "secondary")
        g.edge(seg, "labels", rois, "labels")
        # 四框自己带一片同帧底图：选中它时框就叠在按类着色的测量帧剖面上
        g.edge(colored, "cloud", rois, "backdrop")

        drop_p = g.node("n_drop_p", "gap.drop_non_finite", None, 2, 0, "剔 NaN primary")
        drop_s = g.node("n_drop_s", "gap.drop_non_finite", None, 2, 2, "剔 NaN secondary")
        g.edge(frame_p, "cloud", drop_p, "cloud")
        g.edge(frame_s, "cloud", drop_s, "cloud")

        roll_cfg = common.get("roll_anchored_crop") or {}
        roll = g.node("n_roll", "gap.roll_anchored_crop", {
            "enabled": bool(roll_cfg.get("enabled", False)),
            "halfWidth": float(roll_cfg.get("half_width_mm", 35.0)),
            "halfHeight": float(roll_cfg.get("half_height_mm", 20.0)),
            "maxRollBoxHeight": float(roll_cfg.get("max_roll_box_height_mm", 8.0)),
            "minPointsKept": int(roll_cfg.get("min_points_kept", 50)),
            "usingCamera": str(common.get("using_camera", "Both")),
        }, 4, 1, "跟随零件的裁剪窗")
        g.edge(drop_p, "cloud", roll, "primary")
        g.edge(drop_s, "cloud", roll, "secondary")
        g.edge(rois, "gapLeft", roll, "gapLeft")
        g.edge(rois, "gapRight", roll, "gapRight")

        crop_p, crop_p_port = roll, "primary"
        crop_s, crop_s_port = roll, "secondary"
        merge = g.node("n_merge", "util.merge", None, 5, 1, "合并（secondary 在前）")
        g.edge(roll, "secondary", merge, "a")
        g.edge(roll, "primary", merge, "b")
        merged, merged_port = radius_outlier(merge, "cloud", 6, 1)
    else:
        # ================================================ 配置/模板路径（§2）
        frame_p = g.node("n_frame_p", "gap.to_measurement_frame", None, 1, 0, "换轴 primary")
        frame_s = g.node("n_frame_s", "gap.to_measurement_frame", None, 1, 2, "换轴 secondary")
        g.edge(load, "primary", frame_p, "cloud")
        g.edge(load, "secondary", frame_s, "cloud")

        overall = g.node("n_overall", "gap.overall_roi", {
            "roi": roi_list(common.get("overall_roi")),
            "mode": str(common.get("overall_roi_mode", "fixed")),
            "usingCamera": str(common.get("using_camera", "Both")),
        }, 2, 1, "整体 ROI")
        g.edge(frame_p, "cloud", overall, "primary")
        g.edge(frame_s, "cloud", overall, "secondary")

        crop_p = g.node("n_crop_p", "filter.crop_box2d", CROP_OPEN, 3, 0, "裁 primary")
        crop_s = g.node("n_crop_s", "filter.crop_box2d", CROP_OPEN, 3, 2, "裁 secondary")
        for frame, crop in ((frame_p, crop_p), (frame_s, crop_s)):
            g.edge(frame, "cloud", crop, "cloud")
            g.edge(overall, "box", crop, "box")
        crop_p_port = crop_s_port = "cloud"

        # 合并（secondary 在前，G8）+ 半径离群
        merge = g.node("n_merge", "util.merge", None, 4, 1, "合并（secondary 在前）")
        g.edge(crop_s, "cloud", merge, "a")
        g.edge(crop_p, "cloud", merge, "b")
        merged, merged_port = radius_outlier(merge, "cloud", 5, 1)

        # -- 模板与 ICP -------------------------------------------------------
        candidates = template_candidates(align)
        if len(candidates) > 4:
            raise SystemExit("gap.select_alignment 最多接四个候选，这份配置有 "
                             f"{len(candidates)} 个")
        min_score = int(icp.get("min_score", 60))
        align_nodes = []
        for order, candidate in enumerate(candidates):
            cid = safe_id(str(candidate.get("id", f"t{order}")))
            tpl = g.node(f"n_tpl_{cid}", "gap.load_template", {
                "dir": str(template_dir),
                "left": str(candidate.get("left", "left_template.pcd")),
                "right": str(candidate.get("right", "right_template.pcd")),
            }, 5, 3 + order * 2, f"模板 {cid}")
            params = {
                "templateId": str(candidate.get("id", f"t{order}")),
                "order": order,
                "maxMatchingDist": float(icp.get("max_matching_dist", 1.0)),
                "maxFitnessDist": float(icp.get("max_fitness_dist", 10.0)),
                "maxIterations": int(icp.get("max_iteration_num", 1000)),
                "normalKnn": int(icp.get("num_neighbor", 10)),
                "minScore": min_score,
                "bidirection": bool(icp.get("bidirection_align", False)),
                "globalCoarse": bool(((align.get("robustness") or {}).get("global_coarse", True))),
                "successGuide": bool(align.get("success_guide", False)),
                "segRoi": True,
                "trustTranslation": float((((align.get("robustness") or {})
                                            .get("trust_region") or {})
                                           .get("max_translation_mm", 3.0))),
                "trustRotation": float((((align.get("robustness") or {})
                                         .get("trust_region") or {})
                                        .get("max_rotation_deg", 2.0))),
                "degenerateRatio": float(((align.get("robustness") or {})
                                          .get("degenerate_ratio", 1.0e-3))),
            }
            params.update(candidate_rois(candidate, flush, gap))
            node = g.node(f"n_align_{cid}", "gap.align_template", params, 6, 3 + order * 2,
                          f"ICP {cid}")
            g.edge(merged, merged_port, node, "cloud")
            g.edge(tpl, "left", node, "tplLeft")
            g.edge(tpl, "right", node, "tplRight")
            align_nodes.append(node)

        select = g.node("n_select", "gap.select_alignment", {"minScore": min_score}, 7, 3, "选模板")
        for port, node in zip(("a", "b", "c", "d"), align_nodes):
            g.edge(node, "alignment", select, port)

        rois = g.node("n_rois", "gap.business_rois", {"baseSide": base_side}, 8, 3, "业务 ROI")
        g.edge(select, "alignment", rois, "alignment")

    # -- 四个业务 ROI 的裁剪 --------------------------------------------------
    crops = {}
    for row, (port, title) in enumerate((("flushBase", "裁 flush_base"),
                                         ("flushRef", "裁 flush_ref"),
                                         ("gapLeft", "裁 gap_left"),
                                         ("gapRight", "裁 gap_right"))):
        node = g.node(f"n_crop_{safe_id(port)}", "filter.crop_box2d", CROP_OPEN, 9, row, title)
        g.edge(merged, merged_port, node, "cloud")
        g.edge(rois, port, node, "box")
        crops[port] = node

    # -- 段差 -----------------------------------------------------------------
    # is_left_side：i==0（基准面）时等于 base_side == "left"
    base_is_left = base_side == "left"
    line_dist = float(common.get("line_fit_distance", 0.1))
    segment_points = int(flush.get("segment_points", 0))
    # 模型路径上 align_cloud_ 恒为假，基准线的端点取首尾内点而不是 ROI 交点
    endpoints = "inlier_ends" if args.model else "roi_intersection"
    fit_base = g.node("n_fit_base", "gap.fit_line", {
        "side": "left" if base_is_left else "right",
        "distThresh": line_dist,
        "segmentPoints": segment_points,
        "endpoints": endpoints,
    }, 10, 0, "拟合基准线")
    g.edge(crops["flushBase"], "cloud", fit_base, "cloud")
    g.edge(rois, "flushBase", fit_base, "box")

    if ref_type == "line end":
        fit_ref = g.node("n_fit_ref", "gap.fit_line", {
            "side": "right" if base_is_left else "left",
            "distThresh": line_dist,
            "segmentPoints": segment_points,
            "endpoints": endpoints,
        }, 10, 1, "拟合参考线")
        g.edge(crops["flushRef"], "cloud", fit_ref, "cloud")
        g.edge(rois, "flushRef", fit_ref, "box")
        ref_node, ref_port = fit_ref, "innerEnd"
    elif ref_type == "selected point":  # 取自整片云，不裁 ROI（§3.6）
        sel = g.node("n_sel_ref", "gap.selected_point", None, 10, 1, "选参考点")
        g.edge(merged, merged_port, sel, "cloud")
        g.edge(rois, "flushRef", sel, "box")
        ref_node, ref_port = sel, "point"
    else:  # nearest point：ROI 里离基准线垂距最小的那个点
        near = g.node("n_near_ref", "gap.nearest_to_line", None, 10, 1, "离基准线最近的点")
        g.edge(crops["flushRef"], "cloud", near, "cloud")
        g.edge(fit_base, "line", near, "line")
        ref_node, ref_port = near, "point"

    flush_node = g.node("n_flush", "gap.flush", {
        "offset": float(flush.get("offset", 0.0)),
    }, 11, 0, "段差")
    g.edge(fit_base, "line", flush_node, "baseLine")
    g.edge(ref_node, ref_port, flush_node, "refPoint")

    # -- 间隙 -----------------------------------------------------------------
    radius = gap.get("radius") or {}
    circles = g.node("n_circles", "gap.fit_gap_circles", {
        "distThresh": float(common.get("circle_fit_distance", 0.03)),
        "retryDistance": float(gap.get("circle_fit_retry_distance", 0.0)),
        "nominal": float((gap.get("tolerances") or {}).get("nominal", 0.0)),
        "offset": float(gap.get("offset", 0.0)),
        "leftRadiusFixed": bool(radius.get("fixed_left_circle_radius", False)),
        "leftRadiusValue": float(radius.get("left_circle_radius", 0.0)),
        "leftRadiusMin": float(radius.get("left_circle_radius_min", 0.0)),
        "leftRadiusMax": float(radius.get("left_circle_radius_max", 0.0)),
        "rightRadiusFixed": bool(radius.get("fixed_right_circle_radius", False)),
        "rightRadiusValue": float(radius.get("right_circle_radius", 0.0)),
        "rightRadiusMin": float(radius.get("right_circle_radius_min", 0.0)),
        "rightRadiusMax": float(radius.get("right_circle_radius_max", 0.0)),
        "cameraFallback": bool(gap.get("camera_separated_circle_fallback", True)),
        "selectClosestNominal": bool(
            gap.get("camera_separated_select_closest_nominal",
                    gap.get("camera_separated_select_closest_radius",
                            bool(gap.get("camera_separated_circle_fallback", True))
                            and str(gap.get("camera_separated_preferred_camera", "Both"))
                            == "Both"))),
        "preferredCamera": str(gap.get("camera_separated_preferred_camera", "Both")),
    }, 10, 2, "两侧圆拟合")
    g.edge(merged, merged_port, circles, "merged")
    g.edge(crop_p, crop_p_port, circles, "primary")
    g.edge(crop_s, crop_s_port, circles, "secondary")
    g.edge(rois, "gapLeft", circles, "boxLeft")
    g.edge(rois, "gapRight", circles, "boxRight")

    gap_node = g.node("n_gap", "gap.gap", {
        "definition": str(gap.get("definition", "B")),
        "offset": float(gap.get("offset", 0.0)),
    }, 11, 2, "间隙")
    g.edge(circles, "left", gap_node, "left")
    g.edge(circles, "right", gap_node, "right")
    if str(gap.get("definition", "B")) == "A":
        # 基准线取 gap.flush 那一份：原算法先把段差的垂足并进线段，再拿它定 u
        g.edge(flush_node, "baseLine", gap_node, "baseLine")

    # -- 判定 -----------------------------------------------------------------
    for row, (source, section, title) in enumerate(((flush_node, flush, "判定段差"),
                                                    (gap_node, gap, "判定间隙"))):
        tol = section.get("tolerances") or {}
        judge = g.node(f"n_judge_{row}", "gap.judge", {
            "nominal": float(tol.get("nominal", 0.0)),
            "upper": float(tol.get("up_deviation", 0.0)),
            "lower": float(tol.get("low_deviation", 0.0)),
        }, 12, row * 2, title)
        g.edge(source, "value", judge, "value")

    # -- 旁路：黑盒对照（G5）---------------------------------------------------
    ref_params = {
        "configPath": str(config_path),
        "deriveTemplateDir": False,
        "templateDir": str(template_dir),
        "sampleId": args.sample_id or config_path.parent.name,
    }
    if args.model:
        ref_params["useModel"] = True
        ref_params["modelPath"] = str(Path(args.model))
    ref = g.node("n_ref", "gap.measure_reference", ref_params, 2, 8, "黑盒对照")
    g.edge(load, "primary", ref, "primary")
    g.edge(load, "secondary", ref, "secondary")

    suffix = " · 模型" if args.model else ""
    name = args.name or f"{config_path.parent.name} · {config_path.stem}{suffix}"
    return g.doc(safe_id(args.graph_id or f"gap_{config_path.parent.name}"), name)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", type=Path, help="StandardGap.yml")
    parser.add_argument("point_dir", nargs="?", help="测点目录（里面是两个 LaserProfile_*.pcd）")
    parser.add_argument("--primary", help="直接指定 Master 的 PCD")
    parser.add_argument("--secondary", help="直接指定 Slave 的 PCD")
    parser.add_argument("--template-dir", help="模板目录，默认 <配置目录>/<配置主名>")
    parser.add_argument("--model", help="走模型 ROI 路径：ONNX 模型文件（H5）")
    parser.add_argument("--sample-id", help="写进黑盒对照算子，只做标记")
    parser.add_argument("--graph-id", help="图的 id")
    parser.add_argument("--name", help="图的显示名")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args(argv)

    config = args.config.resolve()
    if not config.is_file():
        raise SystemExit(f"配置文件不存在: {config}")
    doc = build(config, args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"{args.output}  ({len(doc['nodes'])} 节点, {len(doc['edges'])} 连线)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
