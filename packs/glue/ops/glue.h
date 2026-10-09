#pragma once
// glue 包算子之间共用的东西：Bundle / Record 的名字与读写、标定、参数与叠画的小工具。
// 算法本身在 algo/（D11：积木之间共用包内函数，一行算法都不在这里写第二份）。
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include "algo/bead.h"
#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/overlay.h"
#include "lyflow/registry.h"
#include "lyflow_cv/plane_calib.h"

namespace lyflow::packs::glue {

using Json = nlohmann::json;

// ------------------------------------------------------------------ 名字

inline constexpr const char* kPathKind = "glue.Path";
inline constexpr const char* kBeadKind = "glue.Bead";
inline constexpr const char* kPolylineType = "glue.Polyline";
inline constexpr const char* kPathInfoType = "glue.PathInfo";
inline constexpr const char* kStationsType = "glue.Stations";
inline constexpr const char* kBeadInfoType = "glue.BeadInfo";
inline constexpr const char* kBreaksType = "glue.Breaks";
inline constexpr const char* kEdgeType = "glue.Edge";
inline constexpr const char* kVerdictType = "glue.Verdict";
inline constexpr const char* kSynthType = "glue.SynthBreak";
inline constexpr const char* kPoseType = "glue.Pose2D";
inline constexpr const char* kStationMeasureType = "glue.StationMeasure";
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
void registerLocate(Registry& r);
void registerStationCalipers(Registry& r);
void registerSnippets(Registry& r);

// ------------------------------------------------------------------ 图像

/// 端口上的 Image → 8 位单通道 Mat（三通道先按 BT.601 转灰度，与 image.to_gray 同一条路）。
cv::Mat grayOf(const Data& image);

// ------------------------------------------------------------------ JSON 小工具

/// 坐标保留三位小数：下游拿它再算，0.001 px 的舍入无所谓，事件里的 JSON 却小一半。
double round3(double v);
Json pxJson(P2 p);
P2 pxOf(const Json& j);  ///< 不是两个数的数组时返回 (NaN, NaN)
Json numOrNull(double v, int decimals = 3);
double numOf(const Json& j);  ///< null / 缺 → NaN

// ------------------------------------------------------------------ 胶路（glue.Polyline）

/// 从 Record 里读回来的一条折线。s 升序、三个数组等长。
struct Polyline {
  std::vector<double> s;
  std::vector<P2> points;
  std::vector<P2> tangents;
  PolylineView view() const { return PolylineView{&s, &points, &tangents}; }
  /// s 在 [s0, s1] 之间的一段（两端按 s 插值补上）。
  std::vector<P2> between(double s0, double s1) const;
};

Data polylineRecord(const std::vector<double>& s, const std::vector<P2>& points,
                    const std::vector<P2>& tangents);
bool readPolyline(const Data& d, Polyline* out);

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

/// 站 → JSON（数组形式）。width 按 metric 的单位（没接标定 px、接了 mm），widthPx 永远是 px。
struct Metric;
Json stationsJson(const std::vector<Station>& stations, double step, const std::string& form,
                  const Metric& metric);
/// JSON → 站（只还原 s、c、t、n、present、lo、hi、peak；runs 不进 JSON）。
bool readStations(const Json& j, std::vector<Station>* out);

// ------------------------------------------------------------------ 标定

/// 量测的尺子：没接 calib 就是像素，接了按单应映射后两点的距离（glue-plan §2.1）。
struct Metric {
  std::optional<std_image::PlaneCalib> calib;
  std::string unit() const { return calib ? calib->unit : "px"; }
  double distance(P2 a, P2 b) const;
  /// 折线的长度（逐段映射后相加）。
  double length(const std::vector<P2>& pts) const;
};

/// 读可选的 calib 输入。没接 → 像素；接了但格式不对 → bad_input。
Status metricFromInput(const Inputs& inputs, const char* port, Metric* out);
/// bead.info.calib（bead_width 接了标定时写进去的那一份）→ Metric。
Metric metricFromInfo(const Json& info);

// ------------------------------------------------------------------ Measurement

Data measurement(double value, const std::string& unit, const std::string& messageIfMissing);

// ------------------------------------------------------------------ 参数

Param floatParam(const char* name, const char* label, double def, const char* unit,
                 const char* doc, bool advanced = false);
Param vec2Param(const char* name, const char* label, double a, double b, const char* unit,
                std::vector<std::string> components, const char* doc, bool advanced = false);
Param enumParam(const char* name, const char* label, const char* def, const char* doc,
                std::vector<EnumOption> options, bool advanced = false);
Param boolParam(const char* name, const char* label, bool def, const char* doc,
                bool advanced = false);
Param visibleWhen(Param p, const char* param, const char* eq);

// ------------------------------------------------------------------ 叠画

/// 一个 Station 序列里连续有胶的几段（下标区间 [a, b]），画两条胶边时按它断开。
std::vector<std::pair<std::size_t, std::size_t>> presentRuns(const std::vector<Station>& st);
std::string fmt(double v, int decimals = 1);

}  // namespace lyflow::packs::glue
