#pragma once
// glue 包算子之间共用的东西：Bundle / Record 的名字与读写、标定、参数与叠画的小工具。
// 算法本身在 algo/（D11：积木之间共用包内函数，一行算法都不在这里写第二份）。
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include "algo/bead.h"
#include "algo/color.h"
#include "lyflow/calibration.h"
#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/overlay.h"
#include "lyflow/registry.h"
#include "lyflow_cv/compat.h"
#include "lyflow_cv/inspection.h"

namespace lyflow::packs::solution_glue {

using Json = nlohmann::json;

// ------------------------------------------------------------------ 名字

inline constexpr const char* kPathKind = "glue.Path";
inline constexpr const char* kBeadKind = "glue.Bead";
inline constexpr const char* kPathInfoType = "glue.PathInfo";
inline constexpr const char* kStationsType = "glue.Stations";
inline constexpr const char* kBeadInfoType = "glue.BeadInfo";
inline constexpr const char* kBreaksType = "glue.Breaks";
inline constexpr const char* kEdgeType = "glue.Edge";
inline constexpr const char* kVerdictType = "glue.Verdict";
inline constexpr const char* kSynthType = "glue.SynthBreak";
inline constexpr const char* kStationMeasureType = "glue.StationMeasure";
// 1.2 的测量算子（docs/glue-measure-ops.md）
inline constexpr const char* kTrackType = "glue.Track";
/// 胶路没找到时，bead_path 的 info.message 与 judge 的 verdict.message 都是这一句（§2.3 失败语义）。
inline constexpr const char* kNoBeadMessage = "检测区内没找到胶";

void registerBundles(Registry& r);
void registerBeadPath(Registry& r);
void registerTaughtPath(Registry& r);
void registerBeadWidth(Registry& r);
void registerBeadBreaks(Registry& r);
void registerEdgeDistance(Registry& r);
void registerJudge(Registry& r);
void registerSynthBreak(Registry& r);
void registerStationCalipers(Registry& r);
void registerSnippets(Registry& r);
void registerTrackPath(Registry& r);
void registerPolyline(Registry& r);
void registerColorModel(Registry& r);
void registerColorTrack(Registry& r);
void registerMergeColorRuns(Registry& r);

using lyflow::std_image::inspection::badColorImage;
using lyflow::std_image::inspection::badImage;
using lyflow::std_image::inspection::boolParam;
using lyflow::std_image::inspection::ColorMapsView;
using lyflow::std_image::inspection::enumParam;
using lyflow::std_image::inspection::exact;
using lyflow::std_image::inspection::exactPx;
using lyflow::std_image::inspection::floatParam;
using lyflow::std_image::inspection::fmt;
using lyflow::std_image::inspection::grayOf;
using lyflow::std_image::inspection::intParam;
using lyflow::std_image::inspection::kFanScanType;
using lyflow::std_image::inspection::kLateralRunsType;
using lyflow::std_image::inspection::kPolylineType;
using lyflow::std_image::inspection::kPoseType;
using lyflow::std_image::inspection::layStations;
using lyflow::std_image::inspection::MapsView;
using lyflow::std_image::inspection::measurement;
using lyflow::std_image::inspection::numOf;
using lyflow::std_image::inspection::numOrNull;
using lyflow::std_image::inspection::Polyline;
using lyflow::std_image::inspection::polylineRecord;
using lyflow::std_image::inspection::pxJson;
using lyflow::std_image::inspection::pxOf;
using lyflow::std_image::inspection::rangedFloat;
using lyflow::std_image::inspection::readColorMaps;
using lyflow::std_image::inspection::readLine;
using lyflow::std_image::inspection::readMaps;
using lyflow::std_image::inspection::readPolyline;
using lyflow::std_image::inspection::rgbOf;
using lyflow::std_image::inspection::round3;
using lyflow::std_image::inspection::roundTo;
using lyflow::std_image::inspection::StationPose;
using lyflow::std_image::inspection::vec2Param;
using lyflow::std_image::inspection::visibleWhen;

// ------------------------------------------------------------------ Bundle

Data pathBundle(Data line, Json info);
/// 读 glue.Path：line 与 info。
bool readPath(const Data& d, Polyline* line, Json* info);

Data beadBundle(Data line, Json stations, Json info);
struct BeadView {
  Polyline line;
  Json stations;
  Json info;
};
bool readBead(const Data& d, BeadView* out);

// ------------------------------------------------------------------ 逐站（glue.Stations）

/// 站 → JSON（数组形式）。width 使用量测单位，widthPx 使用像素。
struct Metric;
Json stationsJson(const std::vector<Station>& stations, double step, const std::string& form,
                  const Metric& metric);
/// JSON → 站（只还原 s、c、t、n、present、lo、hi、peak；runs 不进 JSON）。
bool readStations(const Json& j, std::vector<Station>* out);

// ------------------------------------------------------------------ 标定

/// 量测的尺子：没接 calib 就是像素，接了按单应映射后两点的距离（glue-plan §2.1）。
struct Metric {
  std::optional<calibration::Plane> calib;
  std::string unit() const { return calib ? calib->unit : "px"; }
  Json calibJson() const;
  double distance(P2 a, P2 b) const;
  /// 折线的长度（逐段映射后相加）。
  double length(const std::vector<P2>& pts) const;
};

/// 读可选的 calib 输入。没接 → 像素；接了但格式不对 → bad_input。
Status metricFromInput(const Inputs& inputs, const char* port, Metric* out);
Status metricFromInfo(const Json& info, Metric* out);

// ------------------------------------------------------------------ 叠画

/// 一个 Station 序列里连续有胶的几段（下标区间 [a, b]），画两条胶边时按它断开。
std::vector<std::pair<std::size_t, std::size_t>> presentRuns(const std::vector<Station>& st);

} // namespace lyflow::packs::solution_glue
