// 端口样例（m6-plan H8 / ADR-0024 那一节末尾）。
//
// Record 端口在 manifest 里只有一个 `type` 字串，所以「`data.inlierCount` 到底存不存在」
// 得翻算子实现才知道 —— 真实任务里这是每个新端口一次的试错。一份样例就够消掉它。
// 样例**不参与任何校验**：它回答的是「这里长什么样」，不是「什么算合法」（那是 contract 的活）。
//
// 每一份都是从真实 run 的输出裁出来的，来源：
//   - R1 那张带 12 个 fallback 的导入图 + KUN10 的一帧（fitQuality / fitQualityPair /
//     labels / refinements / rollCrop / alignment / resultBundle）；
//   - 点 1 的 notch 图 + 同一批数据的一帧（notchQuality）。
// 三份没有现成的真实 run（图里没有这几个算子）：cornerQuality / grooveQuality /
// cameraConsistency —— 它们照着各自 compute 里写 Record 的那几行逐字段构造，
// 字段名与类型都对得上，数值是合理的量级而不是实测值。
//
// 两处**刻意的裁剪**，因为原样放进 manifest 只是体积：
//   - labels 的 row0/row1 实际各 1280 个（一槽一个类 id），这里各留 12 个；
//   - resultBundle 的 fits 实际一维一条（通常 4 条），这里留 line 与 circle 各一条。
// 浮点尾巴都圆过 —— 真实运行里的第 15 位小数对读样例的人没有信息。
#include "gap_ops.h"

namespace lyflow::packs::gap::examples {
namespace {

nlohmann::json parse(const char* text) { return nlohmann::json::parse(text); }

}  // namespace

nlohmann::json fitQuality() {
  return parse(R"json({
    "kind": "Record", "type": "GapFitQuality",
    "data": {
      "model": "line", "pointCount": 113, "inlierCount": 113, "inlierRatio": 1.0,
      "rmsResidualMm": 0.0257, "maxResidualMm": 0.0781,
      "lineDirX": 0.9974, "lineDirY": -0.0714,
      "linePointXMm": 3.0044, "linePointYMm": 171.6769
    }
  })json");
}

nlohmann::json fitQualityPair() {
  return parse(R"json({
    "kind": "Record", "type": "GapFitQualityPair",
    "data": {
      "left": {
        "model": "circle", "pointCount": 63, "inlierCount": 32, "inlierRatio": 0.5079,
        "rmsResidualMm": 0.0143, "maxResidualMm": 0.0291,
        "centerXMm": 7.5699, "centerYMm": 172.8665, "radiusMm": 1.4684,
        "radiusMode": "free", "arcCoverageDeg": 97.4791
      },
      "right": {
        "model": "circle", "pointCount": 40, "inlierCount": 14, "inlierRatio": 0.35,
        "rmsResidualMm": 0.0146, "maxResidualMm": 0.0259,
        "centerXMm": 12.9656, "centerYMm": 172.6968, "radiusMm": 0.5419,
        "radiusMode": "free", "arcCoverageDeg": 195.4365
      }
    }
  })json");
}

nlohmann::json labels() {
  // row0/row1 实际各 1280 个（一槽一个类 id），这里各留 12 个。
  return parse(R"json({
    "kind": "Record", "type": "GapLabels",
    "data": {
      "row0": [0, 0, 0, 2, 2, 2, 3, 3, 5, 5, 6, 6],
      "row1": [0, 0, 2, 2, 2, 3, 3, 5, 5, 6, 6, 0]
    }
  })json");
}

nlohmann::json refinements() {
  // 干净的一帧：精修没有丢掉任何连通块，四个语义段都在。
  return parse(R"json({
    "kind": "Record", "type": "GapRefinements",
    "data": { "text": "", "items": [], "missingSegments": [] }
  })json");
}

nlohmann::json rollCrop() {
  return parse(R"json({
    "kind": "Record", "type": "GapRollCrop",
    "data": {
      "status": "applied", "applied": true,
      "beforePrimary": 1231, "beforeSecondary": 1276,
      "afterPrimary": 212, "afterSecondary": 196,
      "minPointsKept": 50,
      "boxMm": [0.3567, 168.111, 20.3567, 178.111]
    }
  })json");
}

nlohmann::json alignment() {
  return parse(R"json({
    "kind": "Record", "type": "GapAlignment",
    "data": {
      "templateId": "f1", "order": 0, "minScore": 75, "mirrored": true,
      "globalCoarse": true, "globalScore": 92.406, "globalSucceeded": true,
      "left": {
        "success": true, "score": 94.884, "bidirectional": false,
        "transform": [1.0, -0.00266, 0.00395, 0.00266, 1.0, 0.00022, 0.0, 0.0, 1.0],
        "trustRotationDeg": 0.0971, "trustTranslationMm": 2.3373, "trustClamped": false,
        "degenerateRatio": 0.0131, "degenerateLocked": false
      },
      "right": {
        "success": true, "score": 94.884, "bidirectional": false,
        "transform": [1.0, -0.00266, 0.00395, 0.00266, 1.0, 0.00022, 0.0, 0.0, 1.0],
        "trustRotationDeg": 0.0971, "trustTranslationMm": 2.3373, "trustClamped": false,
        "degenerateRatio": 0.0131, "degenerateLocked": false
      },
      "rois": {
        "flushBase": [-5.21, 165.68, 0.32, 175.44],
        "flushRef": [6.98, 166.52, 14.05, 176.8],
        "gapLeft": [-1.53, 166.58, 3.41, 175.44],
        "gapRight": [4.48, 168.24, 9.3, 175.79]
      }
    }
  })json");
}

nlohmann::json cornerQuality() {
  // 构造的（图里没有这个算子的真实 run）：字段照 measure.cpp 里写 Record 的那几行。
  return parse(R"json({
    "kind": "Record", "type": "GapCornerQuality",
    "data": {
      "vertexMm": [12.8413, 173.0512], "originMm": [12.5, 172.8],
      "deltaMm": { "u": 0.3401, "n": 0.2497 },
      "angleDeg": 88.42, "baseDirection": [0.9974, -0.0714],
      "scale": 1.0, "alignmentApplied": true
    }
  })json");
}

nlohmann::json grooveQuality() {
  // 构造的：字段照 groove.cpp 里写 Record 的那几行（含 windowJson 的四个键）。
  return parse(R"json({
    "kind": "Record", "type": "GapGrooveQuality",
    "data": {
      "grooveXMm": 10.4821, "grooveDepthMm": 2.1043, "deepestCount": 5,
      "baseSide": "left", "lowerSurface": "base",
      "flushRawMm": 0.4127, "widthRawMm": 1.8642, "levelDepthMm": 1.4,
      "edgeLeftMm": [9.5512, 173.1204], "edgeRightMm": [11.4154, 173.1204],
      "base": { "yAtGrooveMm": 172.9081, "slope": 0.0143 },
      "ref": { "yAtGrooveMm": 173.3208, "slope": -0.0072 },
      "cameras": {
        "primary": {
          "valid": true,
          "left": { "pointCount": 61, "inlierCount": 58, "yAtGrooveMm": 172.9074, "slope": 0.0141 },
          "right": { "pointCount": 54, "inlierCount": 51, "yAtGrooveMm": 173.3195, "slope": -0.0070 }
        },
        "secondary": {
          "valid": false,
          "left": { "pointCount": 6, "inlierCount": 0, "yAtGrooveMm": null, "slope": null },
          "right": { "pointCount": 3, "inlierCount": 0, "yAtGrooveMm": null, "slope": null }
        }
      },
      "scale": 1.0
    }
  })json");
}

nlohmann::json notchQuality() {
  return parse(R"json({
    "kind": "Record", "type": "GapNotchQuality",
    "data": {
      "baseSide": "left", "flushBase": "left", "camera": "secondary",
      "validCameras": 1, "flushCameras": 1,
      "widthRawMm": 0.7503, "flushRawMm": 0.6737, "levelDepthMm": 0.6, "scale": 1.0,
      "cameras": {
        "primary": {
          "valid": false, "error": "notch_too_shallow",
          "message": "缝角只比基准翼面深 0.600 mm，不到 levelDepth 0.600 mm，这里没有 V 缝",
          "pointCount": 151, "iterations": 1,
          "anchorMm": [13.0192, 187.4978], "cornerDepthMm": 0.6,
          "base": { "pointCount": 57, "inlierCount": 57, "rmsResidualMm": 0.022,
                    "maxResidualMm": 0.0608, "slope": 0.3753,
                    "windowMm": [5.3116, 10.3116], "yAtAnchorMm": 186.8569 },
          "ref": { "valid": false, "error": "", "pointCount": 0, "inlierCount": 0,
                   "rmsResidualMm": null, "maxResidualMm": null, "slope": null,
                   "windowMm": [0.0, 0.0], "yAtMidMm": null },
          "cornerMm": null, "levelYMm": null, "edgeLeftMm": null, "edgeRightMm": null,
          "midXMm": null, "baseYAtMidMm": null, "widthMm": null, "flushRawMm": null
        },
        "secondary": {
          "valid": true, "error": "", "message": "",
          "pointCount": 177, "iterations": 2,
          "anchorMm": [13.4768, 187.6292], "cornerDepthMm": 0.8996,
          "base": { "pointCount": 39, "inlierCount": 39, "rmsResidualMm": 0.0147,
                    "maxResidualMm": 0.0394, "slope": 0.3801,
                    "windowMm": [5.8825, 10.8825], "yAtAnchorMm": 186.6668 },
          "ref": { "valid": true, "error": "", "pointCount": 58, "inlierCount": 58,
                   "rmsResidualMm": 0.0226, "maxResidualMm": 0.0456, "slope": -0.5906,
                   "windowMm": [16.1217, 20.6217], "yAtMidMm": 185.8585 },
          "cornerMm": [13.4768, 187.6292], "levelYMm": 187.0785,
          "edgeLeftMm": [12.8713, 187.0785], "edgeRightMm": [13.6217, 187.0785],
          "midXMm": 13.2465, "baseYAtMidMm": 186.5792,
          "widthMm": 0.7503, "flushRawMm": 0.6737
        }
      }
    }
  })json");
}

nlohmann::json cameraConsistency() {
  // 构造的：字段照 camera_consistency.cpp。evaluated=false 时只有 mode / maxDeltaMm /
  // sampleCount / evaluated / exceeded（外加 reason），四个 delta 都不出现。
  return parse(R"json({
    "kind": "Record", "type": "GapCameraConsistency",
    "data": {
      "mode": "shadow", "maxDeltaMm": 0.5, "sampleCount": 41, "evaluated": true,
      "deltaMedianMm": 0.0182, "deltaMinMm": -0.0431, "deltaMaxMm": 0.0967,
      "exceeded": false
    }
  })json");
}

nlohmann::json resultBundle() {
  // fits 实际一维一条（通常 4 条：flush_base / flush_ref / gap_left / gap_right），
  // 这里留 line 与 circle 各一条 —— 两种 model 的字段集不一样，这才是要看的东西。
  return parse(R"json({
    "kind": "Record", "type": "GapResultBundle",
    "data": {
      "gap": { "value_mm": 3.7838, "status": "success", "unit": "mm", "verdict": "", "message": "" },
      "flush": { "value_mm": 2.3504, "status": "success", "unit": "mm", "verdict": "", "message": "" },
      "roi_source": "model", "crop_status": "applied",
      "fallback": { "choice": "a", "reason": "" }, "fallback_reason": null,
      "effective_roi": {
        "overall": [0.3567, 168.111, 20.3567, 178.111],
        "flush_base": [-5.5561, 171.4385, 5.997, 172.3531],
        "flush_ref": [13.7168, 172.4534, 27.4073, 173.3013],
        "gap_left": [6.0428, 171.3447, 9.0937, 173.8112],
        "gap_right": [12.2126, 172.1289, 14.0778, 175.1592]
      },
      "fits": [
        { "component": "flush_base", "model": "line", "point_count": 113, "inlier_count": 113,
          "inlier_ratio": 1.0, "rms_residual_mm": 0.0257, "max_residual_mm": 0.0781,
          "line_dir_x": 0.9974, "line_dir_y": -0.0714,
          "line_point_x_mm": 3.0044, "line_point_y_mm": 171.6769,
          "center_x_mm": null, "center_y_mm": null, "radius_mm": null,
          "radius_mode": "", "arc_coverage_deg": null },
        { "component": "gap_left", "model": "circle", "point_count": 63, "inlier_count": 32,
          "inlier_ratio": 0.5079, "rms_residual_mm": 0.0143, "max_residual_mm": 0.0291,
          "line_dir_x": null, "line_dir_y": null,
          "line_point_x_mm": null, "line_point_y_mm": null,
          "center_x_mm": 7.5699, "center_y_mm": 172.8665, "radius_mm": 1.4684,
          "radius_mode": "free", "arc_coverage_deg": 97.4791 }
      ],
      "icp": [],
      "point_counts": {
        "input_primary": 1231, "input_secondary": 1276,
        "preprocess_primary": 1231, "preprocess_secondary": 1276,
        "filter_after_left": 398, "filter_after_right": 398,
        "flush_base_roi": 113, "flush_ref_roi": 131,
        "gap_left_roi": 63, "gap_right_roi": 40,
        "roll_crop_applied": 1, "roll_crop_reverted": 0
      },
      "input_point_count": 2507, "preprocessed_point_count": 2507,
      "left_point_count": 398, "right_point_count": 398,
      "consistency_mode": "off", "consistency_gate_failed": false,
      "consistency_retry_attempted": false,
      "consistency_rotation_delta_deg": null, "consistency_translation_delta_mm": null,
      "pack_versions": ["gap@0.2.0", "std-ml@0.1.0", "std-pointcloud@0.1.0"],
      "graph_sha256": "", "timings": [], "runtime_us": 0, "total_runtime_us": 0
    }
  })json");
}

}  // namespace lyflow::packs::gap::examples
