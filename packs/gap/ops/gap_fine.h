#pragma once
// 细粒度算子与积木算子共用的那一份实现（m8-plan L6）。
//
// 细粒度算子的 compute 主体都在 namespace fine 里、以包内函数导出；积木算子不写第二份算法，
// 而是用 Step 把同一个函数原样调一遍 —— 两种建图方式的结果因此逐帧相同（M8a 验收 1）。
// 别的包的算子（filter.crop_box2d、util.merge、filter.radius_outlier、ml.onnx_run）
// 走注册表拿到它们注册的那个 compute 函数指针，同样是同一份实现。
#include <array>
#include <initializer_list>
#include <string>
#include <unordered_map>

#include "gap_ops.h"

namespace lyflow::packs::gap {

// ---------------------------------------------------------------- 细粒度 compute 主体
namespace fine {
Status loadPair(const Inputs&, const ParamView&, Outputs&, ExecContext&);
std::string profilePairKey(const ParamView&);
Status toMeasurementFrame(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status loadTemplate(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status overallRoi(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status businessRois(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status selectedPoint(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status nearestToLine(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status datumWindow(const Inputs&, const ParamView&, Outputs&, ExecContext&);
std::vector<Issue> validateDatumWindow(const ParamView&, const std::set<std::string>&);
Status alignTemplate(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status selectAlignment(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status fitLine(const Inputs&, const ParamView&, Outputs&, ExecContext&);
std::vector<Issue> validateFitLine(const ParamView&, const std::set<std::string>&);
Status fitGapCircles(const Inputs&, const ParamView&, Outputs&, ExecContext&);
std::vector<Issue> validateFitGapCircles(const ParamView&, const std::set<std::string>&);
Status profileTensor(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status labelsFromLogits(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status roiFromLabels(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status dropNonFinite(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status rollAnchoredCrop(const Inputs&, const ParamView&, Outputs&, ExecContext&);
}  // namespace fine

// ---------------------------------------------------------------- 两种 Bundle（L4）
inline constexpr const char* kScanPairKind = "gap.ScanPair";
inline constexpr const char* kRoiSetKind = "gap.RoiSet";
/// RoiSet.info 的 Record 类型。
inline constexpr const char* kRoiInfoType = "GapRoiInfo";

void registerBundles(Registry& r);
/// make / split 两对细粒度算子（bundle_ops.cpp）与七个积木算子（blocks.cpp）。
void registerBundleOps(Registry& r);
void registerBlockOps(Registry& r);
/// 随包的片段（snippets/*.lyflow-snippet.json，m8-plan L14）。
void registerSnippets(Registry& r);

/// ScanPair = primary / secondary / merged 三片测量帧的云。
Data scanPairOf(Data primary, Data secondary, Data merged);

struct ScanView {
  const Data* primary = nullptr;
  const Data* secondary = nullptr;
  const Data* merged = nullptr;
  /// 按名字取一片：merged / primary / secondary。认不出的名字返回 merged。
  const Data& pick(const std::string& which) const;
};
/// 不是 ScanPair 或缺字段返回 false（执行器已经按字段表查过，走到这里说明是内部错误）。
bool readScanPair(const Data& d, ScanView* out);

/// RoiSet 的四个框按角色命名：datum（基准面）、target（参考面）、seamLeft / seamRight（缝的两侧）。
struct RoiView {
  const Data* datum = nullptr;
  const Data* target = nullptr;
  const Data* seamLeft = nullptr;
  const Data* seamRight = nullptr;
  const lyflow::Record* info = nullptr;
  /// datum / target 两个角色框。
  const Data& role(const std::string& which) const;
  /// info.datumSide 是不是 right。
  bool datumRight() const;
};
bool readRoiSet(const Data& d, RoiView* out);

/// info Record：datumSide、source（template | model）、alignment（选中的 GapAlignment 或 null）、
/// overallMm（整体框或跟随裁剪窗，毫米）、cropStatus（GapRollCrop 或 null）。
nlohmann::json roiInfo(bool datumRight, const std::string& source, const nlohmann::json& alignment,
                       const lyflow::Box2D* overall, const nlohmann::json& cropStatus);

Data roiSetOf(Data datum, Data target, Data seamLeft, Data seamRight, nlohmann::json info);

// ------------------------------------------------------------ 方向全部由 RoiSet 推出（L7）
/// 基准面在缝的哪一侧：datum 框中心落在两个缝框中心连线中点的右边就是右侧。
/// 数组是 [x_min, y_min, x_max, y_max]（毫米或米都行，只比相对位置）。
bool datumOnRight(const std::array<double, 4>& datum, const std::array<double, 4>& seamLeft,
                  const std::array<double, 4>& seamRight);
bool datumOnRight(const lyflow::Box2D& datum, const lyflow::Box2D& seamLeft,
                  const lyflow::Box2D& seamRight);
/// GapAlignment 里模板坐标系的四个框（rois.flushBase / gapLeft / gapRight，毫米）。
/// 缺字段返回 false。
bool datumOnRightInRecord(const nlohmann::json& alignmentData, bool* right);

/// 「靠缝那一端」的朝向框：两个缝框中心连线的中点，零尺寸。gap.fit_line 的 toward 只用框中心，
/// 细粒度图接 business_rois / roi_from_labels 的 seam 输出，积木图在算子里现算 —— 同一个函数。
lyflow::Box2D seamTowardBox(const lyflow::Box2D& seamLeft, const lyflow::Box2D& seamRight);

std::array<double, 4> boxToMm(const lyflow::Box2D& box);

// ---------------------------------------------------------------- Step
/// 积木算子里的一步：把一个细粒度算子的 compute 原样调一遍（L6）。参数从那个算子的
/// 默认值起步，再用 copy / set 覆盖；输入端口没给的就是「没接」。
class Step {
 public:
  /// fn 为空时用注册表里 opId 那个算子注册的 compute（跨包的算子走这条）。
  explicit Step(const char* opId, ComputeFn fn = nullptr);

  Step& in(const char* port, const Data& value);
  Step& set(const char* param, Value value);
  /// 从积木算子自己的参数里原样拷同名的那几个（名字相同 = 语义相同，这是约定）。
  Step& copy(const ParamView& from, std::initializer_list<const char*> names);
  /// 细粒度算子声明的参数里，凡是 from 也有的都拷过来。
  Step& copyAll(const ParamView& from);

  /// 跑一遍。失败时 message 前面加上 [label]，portName 换成积木算子自己的 port（可以为空）。
  Status run(ExecContext& ctx, const std::string& label, const char* port = "");

  /// 跑完之后取输出。没有这个端口返回一个空 Data。
  const Data& out(const char* port) const;

 private:
  std::string opId_;
  const OperatorDesc* desc_ = nullptr;
  ComputeFn fn_ = nullptr;
  ParamMap params_;
  std::unordered_map<std::string, Data> inputs_;
  std::unordered_map<std::string, Data> outputs_;
};

/// 拷一个已注册算子的参数声明，积木算子与细粒度算子共用同一份 label / doc / 默认值。
/// advanced 非空时覆盖原值；找不到就抛（注册期的 bug，自检会炸在启动时）。
Param paramOf(const Registry& r, const char* opId, const char* name, int advanced = -1,
              const char* group = nullptr);

}  // namespace lyflow::packs::gap
