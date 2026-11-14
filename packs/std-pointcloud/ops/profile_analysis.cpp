#include <Eigen/Eigenvalues>

#include "p1_common.h"

namespace lyflow::ops {
namespace {
using namespace p1;
using Vec2 = Eigen::Vector2d;
struct ProfileLine {
  Vec2 center = Vec2::Zero(), direction = Vec2::UnitX();
  double minimum = 0, maximum = 0, rms = 0;
  std::size_t count = 0;
};

std::pair<int, int> axes(const std::string& plane) {
  return plane == "xy"   ? std::make_pair(0, 1)
         : plane == "yz" ? std::make_pair(1, 2)
                         : std::make_pair(0, 2);
}

bool fitProfile(const PointCloud& cloud, int x, int y, double minimum, double maximum,
                ProfileLine& result, ExecContext& ctx) {
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 8191) == 0 && ctx.cancelled())
      return false;
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() < minimum || value.x() > maximum)
      continue;
    if (!result.count)
      result.minimum = result.maximum = value.x();
    result.minimum = std::min(result.minimum, value.x());
    result.maximum = std::max(result.maximum, value.x());
    result.center += value;
    ++result.count;
  }
  if (result.count < 3)
    return false;
  result.center /= static_cast<double>(result.count);
  Eigen::Matrix2d covariance = Eigen::Matrix2d::Zero();
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() >= minimum && value.x() <= maximum) {
      const Vec2 delta = value - result.center;
      covariance += delta * delta.transpose();
    }
  }
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> solver(covariance);
  if (solver.info() != Eigen::Success || solver.eigenvalues()[1] <= 0 ||
      solver.eigenvalues()[1] - solver.eigenvalues()[0] <= solver.eigenvalues()[1] * 1e-8)
    return false;
  result.direction = solver.eigenvectors().col(1);
  if (result.direction.x() < 0)
    result.direction *= -1;
  result.center = result.center.cast<float>().cast<double>();
  result.direction = result.direction.cast<float>().cast<double>();
  double squared = 0;
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 8191) == 0 && ctx.cancelled())
      return false;
    const Vec2 value(cloud.xyz[i * 3 + x], cloud.xyz[i * 3 + y]);
    if (value.x() >= minimum && value.x() <= maximum) {
      const Vec2 delta = value - result.center;
      const double cross = delta.x() * result.direction.y() - delta.y() * result.direction.x();
      squared += cross * cross / result.direction.squaredNorm();
    }
  }
  result.rms = std::sqrt(squared / result.count);
  return std::isfinite(result.rms);
}

Line2D nativeLine(const ProfileLine& profile) {
  Line2D line;
  line.unit = Unit2D::Meter;
  for (int i = 0; i < 2; ++i) {
    line.point[i] = static_cast<float>(profile.center[i]);
    line.dir[i] = static_cast<float>(profile.direction[i]);
  }
  return line;
}

} // namespace

void registerProfileAnalysis(Registry& r) {
  OperatorDesc op = descriptor("profile.analyze", "单剖面直线分析",
                               "single scan profile line analysis signal evaluator");
  op.inputs = {{"cloud", "PointCloud", "单剖面点云", "明确选择输入帧的XY/XZ/YZ平面。", true}};
  op.outputs = {{"line", "Line2D", "剖面拟合直线", "所选平面的米制二维直线。", true},
                {"statistics", "Record", "分析", "profile.Line.v1，采样范围/点数/正交RMS。", true}};
  op.params = {choice("plane", "剖面坐标平面", "xz", {{"xy", "XY"}, {"xz", "XZ"}, {"yz", "YZ"}}),
               number("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3, "m"),
               text("frame", "输入参考帧", "profile")};
  op.doc =
      "单条扫描的二维正交最小二乘直线分析；重合/各向同性/少点或残差超限拒绝。无隐式双相机ScanPair、无业务缝型推断。多特征圆/圆弧和复杂SignalEvaluator模式另行组合。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto* cloud = in.get("cloud").asCloud();
    if (!cloud)
      return error("需要单个PointCloud");
    Status s = validate(*cloud, 3);
    if (!s.ok)
      return s;
    if (!frameValid(p.text("frame")) || !std::isfinite(p.number("maxResidual")) ||
        p.number("maxResidual") <= 0 ||
        (p.choice("plane") != "xy" && p.choice("plane") != "xz" && p.choice("plane") != "yz"))
      return error("剖面参数非法", "plane", "bad_param");
    const auto indices = axes(p.choice("plane"));
    ProfileLine line;
    if (!fitProfile(*cloud, indices.first, indices.second, -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(), line, ctx))
      return ctx.cancelled() ? Status::Ok() : error("剖面直线退化或点数不足", "cloud", "no_result");
    if (line.rms > p.number("maxResidual"))
      return error("剖面线拟合残差超过门限", "cloud", "no_result");
    Record stats{"profile.Line.v1",
                 {{"frame", p.text("frame")},
                  {"plane", p.choice("plane")},
                  {"unit", "m"},
                  {"count", line.count},
                  {"rms", line.rms},
                  {"abscissaRange", {line.minimum, line.maximum}}}};
    out.set("line", Data::line2d(nativeLine(line)));
    out.set("statistics", Data::record(std::move(stats)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op =
      descriptor("profile.gap_flush", "单剖面间隙与面差", "single line scan gap flush measurement");
  op.inputs = {{"cloud", "PointCloud", "单扫描点云", "同一参考帧的米制单剖面。", true}};
  op.outputs = {{"gap", "Measurement", "间隙", "右区最小横坐标减左区最大横坐标。", true},
                {"flush", "Measurement", "有符号面差", "referenceX处右拟合面减左拟合面。", true},
                {"left", "Line2D", "左拟合线", "米制剖面坐标。", true},
                {"right", "Line2D", "右拟合线", "米制剖面坐标。", true},
                {"quality", "Record", "质量", "profile.GapFlush.v1。", true}};
  Param left;
  left.name = "leftRange";
  left.label = "左拟合区间";
  left.type = ParamType::Vec2f;
  left.def = Value::vec({-0.02, -0.005});
  left.unit = "m";
  Param right = left;
  right.name = "rightRange";
  right.label = "右拟合区间";
  right.def = Value::vec({0.005, 0.02});
  op.params = {left,
               right,
               choice("plane", "剖面坐标平面", "xz", {{"xy", "XY"}, {"xz", "XZ"}, {"yz", "YZ"}}),
               number("referenceX", "面差横坐标", 0, -1e6, 1e6, "m"),
               number("maxResidual", "最大正交RMS", 0.001, 1e-12, 1e3, "m"),
               number("maxExtrapolation", "最大外推距离", 0.05, 0, 1e3, "m"),
               text("frame", "输入参考帧", "profile")};
  op.doc =
      "使用两个显式不相交横坐标区间拟合单扫描左右表面，分别报告采样边缘间距和同横坐标有符号面差。拒绝少点、竖直线、残差超限和超限外推；圆角/槽/斜投影缝型另用专用算法。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    const auto* cloud = in.get("cloud").asCloud();
    if (!cloud)
      return error("需要单个PointCloud");
    Status s = validate(*cloud, 6);
    if (!s.ok)
      return s;
    const auto l = p.vec2("leftRange"), r = p.vec2("rightRange");
    const double x = p.number("referenceX"), maximum = p.number("maxResidual"),
                 extrapolation = p.number("maxExtrapolation");
    if (!std::isfinite(l[0]) || !std::isfinite(l[1]) || !std::isfinite(r[0]) ||
        !std::isfinite(r[1]) || l[0] >= l[1] || l[1] >= r[0] || r[0] >= r[1] || !std::isfinite(x) ||
        !std::isfinite(maximum) || maximum <= 0 || !std::isfinite(extrapolation) ||
        extrapolation < 0 || !frameValid(p.text("frame")) ||
        (p.choice("plane") != "xy" && p.choice("plane") != "xz" && p.choice("plane") != "yz"))
      return error("剖面区间/外推/质量参数非法", "leftRange", "bad_param");
    const auto indices = axes(p.choice("plane"));
    ProfileLine a, b;
    if (!fitProfile(*cloud, indices.first, indices.second, l[0], l[1], a, ctx) ||
        !fitProfile(*cloud, indices.first, indices.second, r[0], r[1], b, ctx))
      return ctx.cancelled() ? Status::Ok()
                             : error("左右拟合区间需要至少3个非退化点", "cloud", "no_result");
    for (const auto& line : {a, b})
      if (line.rms > maximum || std::abs(line.direction.x()) < 1e-8 ||
          x < line.minimum - extrapolation || x > line.maximum + extrapolation)
        return error("拟合质量、竖直线或外推距离不满足门限", "cloud", "no_result");
    const auto ordinate = [x](const ProfileLine& line) {
      return line.center.y() + (x - line.center.x()) * line.direction.y() / line.direction.x();
    };
    const double gap = b.minimum - a.maximum, flush = ordinate(b) - ordinate(a);
    if (!std::isfinite(gap) || !std::isfinite(flush))
      return error("剖面计算超出范围");
    out.set("gap", Data::measurement(measurement(gap)));
    out.set("flush", Data::measurement(measurement(flush)));
    out.set("left", Data::line2d(nativeLine(a)));
    out.set("right", Data::line2d(nativeLine(b)));
    out.set("quality", Data::record(Record{"profile.GapFlush.v1",
                                           {{"frame", p.text("frame")},
                                            {"plane", p.choice("plane")},
                                            {"unit", "m"},
                                            {"referenceX", x},
                                            {"leftCount", a.count},
                                            {"rightCount", b.count},
                                            {"leftRms", a.rms},
                                            {"rightRms", b.rms}}}));
    return Status::Ok();
  };
  r.addOperator(std::move(op));

  op = descriptor("profile.project_pair", "双剖面已知外参变换",
                  "dual profile projection known extrinsics frame transforms");
  op.inputs = {{"master", "PointCloud", "主扫描", "主扫描帧的米制点云。", true},
               {"slave", "PointCloud", "副扫描", "副扫描帧的米制点云。", true},
               {"masterToCommon", "Transform", "主帧到公共帧", "已标定的刚体外参。", true},
               {"slaveToCommon", "Transform", "副帧到公共帧", "已标定的刚体外参。", true},
               {"commonToOutput", "Transform", "公共帧到输出帧",
                "可选，绕显式旋转中心的变换由调用者构造。", false}};
  op.outputs = {{"master", "PointCloud", "主输出", "输出参考帧。", true},
                {"slave", "PointCloud", "副输出", "同一输出参考帧。", true},
                {"metadata", "Record", "坐标帧", "profile.Frames.v1。", true}};
  op.params = {text("masterFrame", "主参考帧", "master"), text("slaveFrame", "副参考帧", "slave"),
               text("commonFrame", "公共参考帧", "world"),
               text("outputFrame", "输出参考帧", "world")};
  op.doc =
      "只应用调用者显式提供的外参：output←common←master/slave。PointCloud不自带pose元数据，不猜测外参或自动更改配方中心；保留属性并旋转法线。";
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    for (const char* key : {"masterFrame", "slaveFrame", "commonFrame", "outputFrame"})
      if (!frameValid(p.text(key)))
        return error("参考帧必须非空且有界", key, "bad_param");
    Mat4 common = Mat4::Identity();
    if (in.has("commonToOutput")) {
      const auto* value = in.get("commonToOutput").asTransform();
      if (!value || !rigid(*value, common))
        return error("公共帧变换必须为刚体", "commonToOutput");
    } else if (p.text("commonFrame") != p.text("outputFrame")) {
      return error("不同输出帧需要commonToOutput", "commonToOutput");
    }
    PointCloud master, slave;
    for (const auto& pair :
         {std::make_pair("master", "masterToCommon"), std::make_pair("slave", "slaveToCommon")}) {
      const auto* cloud = in.get(pair.first).asCloud();
      const auto* value = in.get(pair.second).asTransform();
      Mat4 matrix;
      if (!cloud || !value || !rigid(*value, matrix))
        return error("点云或已知刚体外参无效", pair.first);
      Status s = validate(*cloud);
      if (!s.ok)
        return s;
      if (!transformed(*cloud, common * matrix,
                       std::string(pair.first) == "master" ? master : slave, ctx))
        return ctx.cancelled() ? Status::Ok() : error("投影坐标或法线超出范围", pair.first);
    }
    out.set("master", Data::cloud(std::move(master)));
    out.set("slave", Data::cloud(std::move(slave)));
    out.set("metadata", Data::record(Record{"profile.Frames.v1",
                                            {{"masterFrame", p.text("masterFrame")},
                                             {"slaveFrame", p.text("slaveFrame")},
                                             {"commonFrame", p.text("commonFrame")},
                                             {"outputFrame", p.text("outputFrame")},
                                             {"unit", "m"}}}));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
