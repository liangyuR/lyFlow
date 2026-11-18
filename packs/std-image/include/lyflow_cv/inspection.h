#pragma once

#include <string>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/overlay.h"
#include "lyflow/registry.h"
#include "lyflow_cv/color_model.h"
#include "lyflow_cv/polyline.h"

namespace lyflow::std_image::inspection {

using Json = nlohmann::json;
using color::ColorMaps;
using line_scan::Field;
using line_scan::P2;
using line_scan::ResponseMaps;

inline constexpr const char* kPolylineType = "glue.Polyline";
inline constexpr const char* kPoseType = "glue.Pose2D";
inline constexpr const char* kResponseMapsKind = "glue.ResponseMaps";
inline constexpr const char* kFrameType = "glue.Frame";
inline constexpr const char* kFanScanType = "glue.FanScan";
inline constexpr const char* kLateralRunsType = "glue.LateralRuns";
inline constexpr const char* kPartEdgesType = "glue.PartEdges";
inline constexpr const char* kColorMapsKind = "glue.ColorMaps";

void registerInspectionBundles(Registry& r);

// ------------------------------------------------------------------ 图像

/// 端口上的 Image → 8 位单通道 Mat（三通道先按 BT.601 转灰度，与 image.to_gray 同一条路）。
cv::Mat grayOf(const Data& image);

/// glue.response 的 maps（Bundle<glue.ResponseMaps>）读成三张按 frame 摆回图像坐标的采样场（零拷贝，Bundle 要活得比它们久）。
struct MapsView {
  Field response, gray, level;
  cv::Rect frame;
};
Data mapsBundle(ResponseMaps maps, cv::Size image);
/// 不是完整的 glue.ResponseMaps（字段缺、不是单通道 f32、尺寸与 frame 对不上）返回 false。
bool readMaps(const Data& d, MapsView* out);
/// 输入图不合格时的执行错误（指到端口）。
Status badImage(const char* port, bool maps);

/// 端口上的 Image → 8 位三通道 RGB Mat（RGBA 去掉 alpha）。单通道、其他位深、预览缩小图返回空 Mat。
cv::Mat rgbOf(const Data& image);
/// 彩色算子的输入图不合格（不是全分辨率 u8 RGB / RGBA）。
Status badColorImage(const char* port);

/// glue.color_map 的 maps（Bundle<glue.ColorMaps>）：score 是按 frame 摆回图像坐标的采样场，state / luma 是块本身。
struct ColorMapsView {
  Field score;
  cv::Mat state, luma;
  cv::Rect frame;
  cv::Size image;
};
Data colorMapsBundle(ColorMaps maps, cv::Size image);
bool readColorMaps(const Data& d, ColorMapsView* out);

// ------------------------------------------------------------------ JSON 小工具

/// 坐标保留三位小数：下游拿它再算，0.001 px 的舍入无所谓，事件里的 JSON 却小一半。
double roundTo(double v, int decimals);
double round3(double v);
Json pxJson(P2 p);
P2 pxOf(const Json& j); ///< 不是两个数的数组时返回 (NaN, NaN)
Json numOrNull(double v, int decimals = 3);
/// 1.2 测量算子的输出不舍入（宿主拿它们做判定，舍入会让门限上的判断与 1.1 不一致）：有限值原样，否则 null。
Json exact(double v);
Json exactPx(P2 p);
double numOf(const Json& j); ///< null / 缺 → NaN

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
/// 1.2 测量算子的 line 端口：Record 类型必须是 glue.Polyline，允许 count = 0（空胶路）。
bool readLine(const Data& d, Polyline* out);

/// 沿胶路布站（glue-measure-ops「约定 / 布站」）：range = [0, 0] 是整条胶路，否则与胶路求交。
struct StationPose {
  double s = 0;
  P2 c, t, n;
};
std::vector<StationPose> layStations(const Polyline& line, double step, double rangeA,
                                     double rangeB, double* usedA, double* usedB);

// ------------------------------------------------------------------ 参数

Param floatParam(const char* name, const char* label, double def, const char* unit, const char* doc,
                 bool advanced = false);
Param vec2Param(const char* name, const char* label, double a, double b, const char* unit,
                std::vector<std::string> components, const char* doc, bool advanced = false);
Param enumParam(const char* name, const char* label, const char* def, const char* doc,
                std::vector<EnumOption> options, bool advanced = false);
Param boolParam(const char* name, const char* label, bool def, const char* doc,
                bool advanced = false);
Param intParam(const char* name, const char* label, int def, const char* doc, int lo, int hi,
               bool advanced = false);
/// 带硬边界的 float 参数（lo / hi 给 NaN 表示那一侧不限）。
Param rangedFloat(const char* name, const char* label, double def, const char* unit,
                  const char* doc, double lo, double hi, bool advanced = false);
Param visibleWhen(Param p, const char* param, const char* eq);

Data measurement(double value, const std::string& unit, const std::string& messageIfMissing);

std::string fmt(double v, int decimals = 1);

} // namespace lyflow::std_image::inspection
