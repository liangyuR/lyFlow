// gap 算子包的单测。跟 LyFlow core 共用同一个 doctest 目标（ADR-0013）。
// 这里只测「必须逐条复刻」的那几条语义（计划 §3），拟合本身是复用的库函数，不重测。
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap {
void registerPackOps(Registry& r);
}

namespace {

using namespace lyflow;

/// 不取消、不记进度的最小 ExecContext。
class NullContext final : public ExecContext {
 public:
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return baseDir_; }
  int threadBudget() const override { return 1; }

 private:
  std::filesystem::path baseDir_;
};

const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::gap::registerPackOps(reg);
    return reg;
  }();
  return r;
}

/// 直接调一个算子的 compute：参数先铺默认值，再用 overrides 覆盖。
struct Call {
  std::unordered_map<std::string, Data> inputs;
  std::unordered_map<std::string, Data> outputs;
  ParamMap params;
  Status status;

  Status run(const std::string& opId, const std::unordered_map<std::string, Value>& overrides = {}) {
    const OperatorDesc* op = packRegistry().find(opId);
    REQUIRE(op != nullptr);
    for (const Param& p : op->params) params[p.name] = p.def;
    for (const auto& [k, v] : overrides) params[k] = v;
    NullContext ctx;
    const std::filesystem::path base;
    ParamView view(params, base);
    Inputs in(inputs);
    Outputs out(outputs);
    status = op->compute(in, view, out, ctx);
    return status;
  }

  const Data& out(const std::string& port) { return outputs[port]; }
};

Box2D box(float xMin, float yMin, float xMax, float yMax) {
  Box2D b;
  b.min[0] = xMin;
  b.min[1] = yMin;
  b.max[0] = xMax;
  b.max[1] = yMax;
  return b;
}

/// n 个共线的点。ascend=false 时 x 递减，用来翻转 §3.5 的截取方向。
PointCloud lineCloud(std::size_t n, bool ascend) {
  PointCloud c;
  for (std::size_t i = 0; i < n; ++i) {
    const auto k = static_cast<float>(ascend ? i : n - 1 - i);
    c.push(k * 0.001f, 0.5f + k * 0.0001f, 0.0f);
  }
  return c;
}

}  // namespace

// gap.crop_box 已删除：生成的图改用 filter.crop_box2d(bounds=open)，
// 开闭区间的断言随之搬到 packs/std-pointcloud/tests/test_2d_ops.cpp。

TEST_CASE("gap.fit_line 的截取方向：ascend × side 四种组合") {
  // 100 个共线点、segmentPoints=10：留下的必是靠缝隙那一端的十个。
  // ascend == isLeft 留尾巴，否则留头（§3.5）。
  struct Case {
    bool ascend;
    const char* side;
    bool expectTail;
  };
  const Case cases[4] = {
      {true, "left", true},
      {false, "left", false},
      {true, "right", false},
      {false, "right", true},
  };
  for (const Case& c : cases) {
    CAPTURE(c.ascend);
    CAPTURE(c.side);
    Call call;
    call.inputs["cloud"] = Data::cloud(lineCloud(100, c.ascend));
    call.inputs["box"] = Data::box2d(box(-1.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(call.run("gap.fit_line", {
                                         {"side", Value::text(c.side)},
                                         {"distThresh", Value::number(0.5)},
                                         {"segmentPoints", Value::integer(10)},
                                     })
                .ok);
    const Indices* inliers = call.out("inliers").asIndices();
    REQUIRE(inliers != nullptr);
    REQUIRE(inliers->values.size() == 10);
    const std::int32_t lo = inliers->values.front();
    const std::int32_t hi = inliers->values.back();
    if (c.expectTail) {
      CHECK(lo == 90);
      CHECK(hi == 99);
    } else {
      CHECK(lo == 0);
      CHECK(hi == 9);
    }

    // innerEnd 是内点里靠缝隙那一端的真实云点（§3.6）：左侧板子永远取 x 最大的
    // 那头、右侧取 x 最小的那头，与点在文件里的存放顺序（ascend）无关。
    const Point2D* inner = call.out("innerEnd").asPoint2D();
    REQUIRE(inner != nullptr);
    CHECK(inner->p[0] ==
          doctest::Approx(std::string(c.side) == "left" ? 0.099f : 0.0f).epsilon(0.01));
  }
}

TEST_CASE("gap.fit_line 内点少于 segmentPoints 时不截取") {
  Call call;
  call.inputs["cloud"] = Data::cloud(lineCloud(20, true));
  call.inputs["box"] = Data::box2d(box(-1.0f, 0.0f, 1.0f, 1.0f));
  REQUIRE(call.run("gap.fit_line", {{"distThresh", Value::number(0.5)},
                                    {"segmentPoints", Value::integer(500)}})
              .ok);
  CHECK(call.out("inliers").asIndices()->values.size() == 20);
}

TEST_CASE("gap.flush 取绝对值并加 offset，符号不参与") {
  Line2D base;           // y = 0 那条线
  base.point[0] = 0.0f;
  base.point[1] = 0.0f;
  base.dir[0] = 1.0f;
  base.dir[1] = 0.0f;
  Point2D ref;
  ref.p[0] = 0.5f;
  ref.p[1] = 0.002f;     // 线上方 2 mm

  Call above;
  above.inputs["baseLine"] = Data::line2d(base);
  above.inputs["refPoint"] = Data::point2d(ref);
  REQUIRE(above.run("gap.flush", {{"offset", Value::number(0.4)}}).ok);
  const Measurement* m = above.out("value").asMeasurement();
  REQUIRE(m != nullptr);
  CHECK(m->value == doctest::Approx(2.4));

  ref.p[1] = -0.002f;    // 线下方 2 mm：绝对值一样
  Call below;
  below.inputs["baseLine"] = Data::line2d(base);
  below.inputs["refPoint"] = Data::point2d(ref);
  REQUIRE(below.run("gap.flush", {{"offset", Value::number(0.4)}}).ok);
  CHECK(below.out("value").asMeasurement()->value == doctest::Approx(2.4));
}

TEST_CASE("gap.gap definition A 沿基准线方向量切线距离") {
  Circle2D left;
  left.center[0] = 0.0f;
  left.center[1] = 0.17f;
  left.radius = 0.002f;
  Circle2D right;
  right.center[0] = 0.01f;
  right.center[1] = 0.17f;
  right.radius = 0.003f;
  Line2D base;
  base.hasSegment = true;
  base.start[0] = -0.005f;
  base.start[1] = 0.17f;
  base.end[0] = 0.005f;
  base.end[1] = 0.17f;
  base.dir[0] = 1.0f;

  Call call;
  call.inputs["left"] = Data::circle2d(left);
  call.inputs["right"] = Data::circle2d(right);
  call.inputs["baseLine"] = Data::line2d(base);
  REQUIRE(call.run("gap.gap", {{"definition", Value::text("A")},
                               {"offset", Value::number(0.4)}})
              .ok);
  // (0.01 - 0) - 0.002 - 0.003 = 0.005 m = 5 mm，加 offset
  CHECK(call.out("value").asMeasurement()->value == doctest::Approx(5.4));

  // 端点顺序反过来也一样：u 被强制指向 +x
  std::swap(base.start[0], base.end[0]);
  Call flipped;
  flipped.inputs["left"] = Data::circle2d(left);
  flipped.inputs["right"] = Data::circle2d(right);
  flipped.inputs["baseLine"] = Data::line2d(base);
  REQUIRE(flipped.run("gap.gap", {{"definition", Value::text("A")},
                                  {"offset", Value::number(0.4)}})
              .ok);
  CHECK(flipped.out("value").asMeasurement()->value == doctest::Approx(5.4));
}

TEST_CASE("gap.gap definition A 两条切线交叉时报错") {
  Circle2D left;
  left.center[0] = 0.0f;
  left.radius = 0.006f;
  Circle2D right;
  right.center[0] = 0.01f;
  right.radius = 0.006f;
  Line2D base;
  base.hasSegment = true;
  base.end[0] = 0.01f;
  base.dir[0] = 1.0f;

  Call call;
  call.inputs["left"] = Data::circle2d(left);
  call.inputs["right"] = Data::circle2d(right);
  call.inputs["baseLine"] = Data::line2d(base);
  const Status s = call.run("gap.gap", {{"definition", Value::text("A")}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "invalid_geometry");
}

TEST_CASE("gap.judge 的判定字段") {
  const auto judge = [](double value, double margin, bool patrol) {
    Measurement m;
    m.value = value;
    m.ok = true;
    Call call;
    call.inputs["value"] = Data::measurement(std::move(m));
    REQUIRE(call.run("gap.judge", {{"nominal", Value::number(3.5)},
                                   {"upper", Value::number(1.0)},
                                   {"lower", Value::number(-1.0)},
                                   {"margin", Value::number(margin)},
                                   {"patrol", Value::boolean(patrol)}})
                .ok);
    return call.out("value").asMeasurement()->verdict;
  };
  CHECK(judge(3.5, 0.0, false) == "ok");
  CHECK(judge(4.6, 0.0, false) == "high");
  CHECK(judge(2.4, 0.0, false) == "low");
  CHECK(judge(4.45, 0.1, false) == "margin");
  // 巡检模式：超差也只标 margin
  CHECK(judge(4.6, 0.0, true) == "margin");

  Measurement failed;
  failed.value = std::numeric_limits<double>::quiet_NaN();
  failed.ok = false;
  Call call;
  call.inputs["value"] = Data::measurement(std::move(failed));
  REQUIRE(call.run("gap.judge").ok);
  CHECK(call.out("value").asMeasurement()->verdict == "fail");
}

TEST_CASE("gap.business_rois 只变换对角两角点，base_side=right 时端口互换") {
  // 单位变换 + 四个互不相同的 ROI，直接看端口对上了没有
  nlohmann::json identity = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  Record rec;
  rec.type = "GapAlignment";
  rec.data = nlohmann::json{
      {"left", {{"transform", identity}}},
      {"right", {{"transform", identity}}},
      {"rois",
       {{"flushBase", {0.0, 0.0, 1.0, 1.0}},
        {"gapLeft", {2.0, 0.0, 3.0, 1.0}},
        {"flushRef", {4.0, 0.0, 5.0, 1.0}},
        {"gapRight", {6.0, 0.0, 7.0, 1.0}}}},
  };

  Call left;
  left.inputs["alignment"] = Data::record(rec);
  REQUIRE(left.run("gap.business_rois", {{"baseSide", Value::text("left")}}).ok);
  CHECK(left.out("flushBase").asBox2D()->min[0] == doctest::Approx(0.0));
  CHECK(left.out("flushRef").asBox2D()->min[0] == doctest::Approx(0.004));

  Call right;
  right.inputs["alignment"] = Data::record(rec);
  REQUIRE(right.run("gap.business_rois", {{"baseSide", Value::text("right")}}).ok);
  // 互换之后基准面那一格拿的是配置里的 flush_base，仍然从 flushBase 端口出来
  CHECK(right.out("flushBase").asBox2D()->min[0] == doctest::Approx(0.0));
  CHECK(right.out("flushRef").asBox2D()->min[0] == doctest::Approx(0.004));
  // 间隙两侧不受 base_side 影响
  CHECK(right.out("gapLeft").asBox2D()->min[0] == doctest::Approx(0.002));
  CHECK(right.out("gapRight").asBox2D()->min[0] == doctest::Approx(0.006));
}

TEST_CASE("gap.overall_roi 的 auto_center 保留宽高、中心取两片云中位数的中点") {
  PointCloud a;
  for (int i = 0; i < 5; ++i) a.push(0.01f * static_cast<float>(i), 0.1f, 0.0f);
  PointCloud b;
  for (int i = 0; i < 5; ++i) b.push(0.1f + 0.01f * static_cast<float>(i), 0.2f, 0.0f);

  Call call;
  call.inputs["primary"] = Data::cloud(std::move(a));
  call.inputs["secondary"] = Data::cloud(std::move(b));
  REQUIRE(call.run("gap.overall_roi", {{"mode", Value::text("auto_center")},
                                       {"roi", Value::vec({-10.0, -20.0, 10.0, 20.0})}})
              .ok);
  const Box2D* out = call.out("box").asBox2D();
  REQUIRE(out != nullptr);
  // 中位数：a 是 (20 mm, 100 mm)，b 是 (120 mm, 200 mm) -> 中点 (70, 150) mm
  CHECK(out->min[0] == doctest::Approx(0.060));
  CHECK(out->max[0] == doctest::Approx(0.080));
  CHECK(out->min[1] == doctest::Approx(0.130));
  CHECK(out->max[1] == doctest::Approx(0.170));
}

// ------------------------------------------------------------ gap.corner_vertex

namespace {

/// 一条带端点的 2D 线段。dir 由两端点算出，正负与端点顺序一致 —— 算子不该依赖它。
Line2D seg(float x0, float y0, float x1, float y1) {
  Line2D l;
  l.hasSegment = true;
  l.start[0] = x0;
  l.start[1] = y0;
  l.end[0] = x1;
  l.end[1] = y1;
  l.point[0] = x0;
  l.point[1] = y0;
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float n = std::sqrt(dx * dx + dy * dy);
  l.dir[0] = dx / n;
  l.dir[1] = dy / n;
  return l;
}

}  // namespace

TEST_CASE("gap.corner_vertex 交点相对金件顶点沿基准面方向分解") {
  // 左翼面 y = 0.17 那条横线，右翼面 x = 0.01 那条竖线 → 交点 (0.01, 0.17)
  const Line2D left = seg(-0.005f, 0.17f, 0.005f, 0.17f);
  const Line2D right = seg(0.01f, 0.17f, 0.01f, 0.19f);
  Line2D base = left;  // u = +x

  Call call;
  call.inputs["lineLeft"] = Data::line2d(left);
  call.inputs["lineRight"] = Data::line2d(right);
  call.inputs["baseLine"] = Data::line2d(base);
  // 金件顶点 (8 mm, 170 mm) → 位移沿 u 是 2 mm，沿 n 是 0
  REQUIRE(call.run("gap.corner_vertex", {{"originX", Value::number(8.0)},
                                         {"originY", Value::number(170.0)},
                                         {"gapOffset", Value::number(0.4)}})
              .ok);
  const Point2D* v = call.out("vertex").asPoint2D();
  REQUIRE(v != nullptr);
  CHECK(v->p[0] == doctest::Approx(0.01));
  CHECK(v->p[1] == doctest::Approx(0.17));
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(2.4));
  CHECK(call.out("flush").asMeasurement()->value == doctest::Approx(0.0));
  CHECK(call.out("angle").asMeasurement()->value == doctest::Approx(90.0));
  CHECK(call.out("angle").asMeasurement()->unit == "deg");

  // scale 是读数增益：顶点位移比真实间隙大，标定用
  Call scaled;
  scaled.inputs = call.inputs;
  REQUIRE(scaled.run("gap.corner_vertex", {{"originX", Value::number(8.0)},
                                           {"originY", Value::number(170.0)},
                                           {"scale", Value::number(0.5)}})
              .ok);
  CHECK(scaled.out("gap").asMeasurement()->value == doctest::Approx(1.0));
}

TEST_CASE("gap.corner_vertex 的夹角取两条翼面从顶点出发的方向，60° 不会变成 120°") {
  // 两条翼面都从顶点往左走，夹角 60°。直接拿 dir 点乘会得到 120°。
  const Line2D left = seg(0.0f, 0.0f, -0.01f, 0.0f);
  const Line2D right = seg(0.0f, 0.0f, -0.005f, -0.0086602540f);

  Call call;
  call.inputs["lineLeft"] = Data::line2d(left);
  call.inputs["lineRight"] = Data::line2d(right);
  REQUIRE(call.run("gap.corner_vertex").ok);
  CHECK(call.out("angle").asMeasurement()->value == doctest::Approx(60.0).epsilon(0.01));
}

TEST_CASE("gap.corner_vertex 近平行与夹角出界都判失败") {
  Call parallel;
  parallel.inputs["lineLeft"] = Data::line2d(seg(0.0f, 0.0f, 0.01f, 0.0f));
  parallel.inputs["lineRight"] = Data::line2d(seg(0.0f, 0.001f, 0.01f, 0.001f));
  const Status ps = parallel.run("gap.corner_vertex");
  CHECK_FALSE(ps.ok);
  CHECK(ps.code == "invalid_geometry");

  Call narrow;
  narrow.inputs["lineLeft"] = Data::line2d(seg(-0.005f, 0.17f, 0.005f, 0.17f));
  narrow.inputs["lineRight"] = Data::line2d(seg(0.01f, 0.17f, 0.01f, 0.19f));
  const Status ns = narrow.run("gap.corner_vertex", {{"maxAngle", Value::number(45.0)}});
  CHECK_FALSE(ns.ok);
  CHECK(ns.code == "invalid_geometry");
}

TEST_CASE("gap.corner_vertex 用 ICP 变换把模板坐标系里的金件顶点搬到样本上") {
  const Line2D left = seg(-0.005f, 0.17f, 0.005f, 0.17f);
  const Line2D right = seg(0.01f, 0.17f, 0.01f, 0.19f);

  Record alignment;
  alignment.type = "GapAlignment";
  // 沿 +x 平移 2 mm：模板里的 (8, 170) 搬到样本上就是 (10, 170) = 交点 → 间隙读 0
  alignment.data["left"]["transform"] = {1.0, 0.0, 0.002, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

  Call call;
  call.inputs["lineLeft"] = Data::line2d(left);
  call.inputs["lineRight"] = Data::line2d(right);
  call.inputs["baseLine"] = Data::line2d(left);
  call.inputs["alignment"] = Data::record(alignment);
  REQUIRE(call.run("gap.corner_vertex",
                   {{"originX", Value::number(8.0)}, {"originY", Value::number(170.0)}})
              .ok);
  // float 舍入下 1e-6 mm 量级的残差，绝对比较
  CHECK(std::fabs(call.out("gap").asMeasurement()->value) < 1e-3);
  const Record* q = call.out("quality").asRecord();
  REQUIRE(q != nullptr);
  CHECK(q->type == "GapCornerQuality");
  CHECK(q->data["alignmentApplied"].get<bool>());
}

// ------------------------------------------------------------ gap.point_offset

namespace {

Point2D pt(float x, float y) {
  Point2D p;
  p.p[0] = x;
  p.p[1] = y;
  return p;
}

}  // namespace

TEST_CASE("gap.point_offset 把 b 相对 a 的位移分解到 x/y 两个轴") {
  const Point2D a = pt(0.010f, 0.190f);
  const Point2D b = pt(0.0125f, 0.1915f);

  Call call;
  call.inputs["a"] = Data::point2d(a);
  call.inputs["b"] = Data::point2d(b);
  REQUIRE(call.run("gap.point_offset").ok);
  CHECK(call.out("dx").asMeasurement()->value == doctest::Approx(2.5).epsilon(1e-3));
  CHECK(call.out("dy").asMeasurement()->value == doctest::Approx(1.5).epsilon(1e-3));
  CHECK(call.out("distance").asMeasurement()->value ==
        doctest::Approx(std::hypot(2.5, 1.5)).epsilon(1e-3));

  const Line2D* segment = call.out("segment").asLine2D();
  REQUIRE(segment != nullptr);
  CHECK(segment->hasSegment);
  CHECK(segment->start[0] == doctest::Approx(a.p[0]));
  CHECK(segment->start[1] == doctest::Approx(a.p[1]));
  CHECK(segment->end[0] == doctest::Approx(b.p[0]));
  CHECK(segment->end[1] == doctest::Approx(b.p[1]));
}

TEST_CASE("gap.point_offset 交换 a/b 翻符号，absDx/absDy 拉回正值") {
  const Point2D a = pt(0.0125f, 0.1915f);
  const Point2D b = pt(0.010f, 0.190f);

  Call call;
  call.inputs["a"] = Data::point2d(a);
  call.inputs["b"] = Data::point2d(b);
  REQUIRE(call.run("gap.point_offset").ok);
  CHECK(call.out("dx").asMeasurement()->value == doctest::Approx(-2.5).epsilon(1e-3));
  CHECK(call.out("dy").asMeasurement()->value == doctest::Approx(-1.5).epsilon(1e-3));

  Call abs;
  abs.inputs = call.inputs;
  REQUIRE(abs.run("gap.point_offset",
                  {{"absDx", Value::boolean(true)}, {"absDy", Value::boolean(true)}})
              .ok);
  CHECK(abs.out("dx").asMeasurement()->value == doctest::Approx(2.5).epsilon(1e-3));
  CHECK(abs.out("dy").asMeasurement()->value == doctest::Approx(1.5).epsilon(1e-3));
}

TEST_CASE("gap.point_offset 的 scale 与 offset 是线性的") {
  const Point2D a = pt(0.010f, 0.190f);
  const Point2D b = pt(0.0125f, 0.1915f);

  Call call;
  call.inputs["a"] = Data::point2d(a);
  call.inputs["b"] = Data::point2d(b);
  REQUIRE(call.run("gap.point_offset", {{"scaleDx", Value::number(2.0)},
                                        {"dxOffset", Value::number(0.1)},
                                        {"dyOffset", Value::number(-0.2)}})
              .ok);
  CHECK(call.out("dx").asMeasurement()->value == doctest::Approx(5.1).epsilon(1e-3));
  CHECK(call.out("dy").asMeasurement()->value == doctest::Approx(1.3).epsilon(1e-3));
}

namespace {

float mmf(double v) { return static_cast<float>(v / 1000.0); }

constexpr double kGrooveXcMm = -3.0;
constexpr double kGrooveY0Mm = 230.0;
constexpr double kGrooveSlopeK = 0.5;

PointCloud grooveCloud(double gapHalfMm, double flushMm) {
  PointCloud c;
  const auto baseY = [](double xMm) {
    return kGrooveY0Mm + kGrooveSlopeK * (xMm - kGrooveXcMm);
  };
  for (int i = 0; i <= 200; ++i) {
    const double xMm = kGrooveXcMm - 10.0 + 0.1 * i;
    if (xMm < kGrooveXcMm - gapHalfMm) {
      c.push(mmf(xMm), mmf(baseY(xMm)), 0.0f);
    } else if (xMm > kGrooveXcMm + gapHalfMm) {
      c.push(mmf(xMm), mmf(baseY(xMm) + flushMm), 0.0f);
    }
  }
  for (int i = 1; i <= 9; ++i) {
    const double d = 0.2 * i;
    const double xl = kGrooveXcMm - gapHalfMm;
    const double xr = kGrooveXcMm + gapHalfMm;
    c.push(mmf(xl), mmf(baseY(xl) + d), 0.0f);
    c.push(mmf(xr), mmf(baseY(xr) + flushMm + d), 0.0f);
  }
  for (int j = -2; j <= 2; ++j) {
    const double xMm = kGrooveXcMm + 0.05 * j;
    c.push(mmf(xMm), mmf(baseY(xMm) + 2.5), 0.0f);
  }
  return c;
}

PointCloud flatCloud() {
  PointCloud c;
  for (int i = 0; i <= 200; ++i) {
    const double xMm = kGrooveXcMm - 10.0 + 0.1 * i;
    c.push(mmf(xMm), mmf(kGrooveY0Mm + kGrooveSlopeK * (xMm - kGrooveXcMm)), 0.0f);
  }
  return c;
}

PointCloud shiftedInY(const PointCloud& src, double dyMm) {
  PointCloud c;
  for (std::size_t i = 0; i < src.pointCount(); ++i) {
    c.push(src.xyz[3 * i], src.xyz[3 * i + 1] + mmf(dyMm), src.xyz[3 * i + 2]);
  }
  return c;
}

double nominalFlushMm(double flushMm) {
  return -flushMm * std::cos(std::atan(kGrooveSlopeK));
}

}  // namespace

TEST_CASE("gap.groove_joint 量出槽宽与面差") {
  const PointCloud cloud = grooveCloud(0.15, 0.4);
  Call call;
  call.inputs["primary"] = Data::cloud(cloud);
  call.inputs["secondary"] = Data::cloud(cloud);
  REQUIRE(call.run("gap.groove_joint").ok);

  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
  CHECK(call.out("flush").asMeasurement()->value ==
        doctest::Approx(nominalFlushMm(0.4)).epsilon(0.02));

  const Point2D* groove = call.out("groove").asPoint2D();
  REQUIRE(groove != nullptr);
  CHECK(std::fabs(static_cast<double>(groove->p[0]) - kGrooveXcMm / 1000.0) < 0.0002);

  const Record* q = call.out("quality").asRecord();
  REQUIRE(q != nullptr);
  CHECK(q->type == "GapGrooveQuality");
  CHECK(q->data["lowerSurface"].get<std::string>() == "ref");
  CHECK(q->data["cameras"]["primary"]["valid"].get<bool>());

  const Record* qb = call.out("qualityBase").asRecord();
  REQUIRE(qb != nullptr);
  CHECK(qb->type == "GapFitQuality");
  CHECK(qb->data["model"].get<std::string>() == "line");
  const Record* qr = call.out("qualityRef").asRecord();
  REQUIRE(qr != nullptr);
  CHECK(qr->type == "GapFitQuality");
}

TEST_CASE("gap.groove_joint 换基准侧只翻 flush 的符号") {
  const PointCloud cloud = grooveCloud(0.15, 0.4);
  Call call;
  call.inputs["primary"] = Data::cloud(cloud);
  call.inputs["secondary"] = Data::cloud(cloud);
  REQUIRE(call.run("gap.groove_joint", {{"baseSide", Value::text("right")}}).ok);
  CHECK(call.out("flush").asMeasurement()->value ==
        doctest::Approx(-nominalFlushMm(0.4)).epsilon(0.02));
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
  CHECK(call.out("quality").asRecord()->data["lowerSurface"].get<std::string>() == "base");
}

TEST_CASE("gap.groove_joint 没有槽就报 groove_not_found") {
  Call call;
  call.inputs["primary"] = Data::cloud(flatCloud());
  call.inputs["secondary"] = Data::cloud(flatCloud());
  const Status s = call.run("gap.groove_joint");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "groove_not_found");
}

TEST_CASE("gap.groove_joint 只有一台相机也能测，另一台标 valid=false") {
  const PointCloud cloud = grooveCloud(0.15, 0.4);
  Call call;
  call.inputs["primary"] = Data::cloud(cloud);
  call.inputs["secondary"] = Data::cloud(PointCloud{});
  REQUIRE(call.run("gap.groove_joint").ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
  CHECK(call.out("flush").asMeasurement()->value ==
        doctest::Approx(nominalFlushMm(0.4)).epsilon(0.02));
  const Record* q = call.out("quality").asRecord();
  REQUIRE(q != nullptr);
  CHECK(q->data["cameras"]["primary"]["valid"].get<bool>());
  CHECK_FALSE(q->data["cameras"]["secondary"]["valid"].get<bool>());
}

TEST_CASE("gap.groove_joint 两台相机整体差 0.2 mm，平均之后 flush 不动") {
  const PointCloud cloud = grooveCloud(0.15, 0.4);
  Call call;
  call.inputs["primary"] = Data::cloud(cloud);
  call.inputs["secondary"] = Data::cloud(shiftedInY(cloud, 0.2));
  REQUIRE(call.run("gap.groove_joint").ok);
  CHECK(std::fabs(call.out("flush").asMeasurement()->value - nominalFlushMm(0.4)) < 0.02);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.3).epsilon(0.05));
}

TEST_CASE("gap.groove_joint 的 scale 与两个 offset 是线性的") {
  const PointCloud cloud = grooveCloud(0.15, 0.4);
  Call call;
  call.inputs["primary"] = Data::cloud(cloud);
  call.inputs["secondary"] = Data::cloud(cloud);
  REQUIRE(call.run("gap.groove_joint", {{"scale", Value::number(2.0)},
                                        {"gapOffset", Value::number(0.1)},
                                        {"flushOffset", Value::number(0.05)}})
              .ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(0.7).epsilon(0.05));
  CHECK(call.out("flush").asMeasurement()->value ==
        doctest::Approx(nominalFlushMm(0.4) + 0.05).epsilon(0.05));
}

// ------------------------------------------------------------------ gap.notch_width
namespace {

float notchMm(double v) { return static_cast<float>(v / 1000.0); }

constexpr double kNotchXcMm = 13.0;
constexpr double kNotchY0Mm = 187.0;
constexpr double kNotchSlopeA = 0.4;   // 基准翼面：向缝越走越深
constexpr double kNotchEdgeDrop = 0.6; // 基准侧圆边在最后 1 mm 里额外下沉的深度
constexpr double kNotchWallHeight = 1.2;

/// 基准翼面 + 1 mm 陡降的圆边 | 对面的竖直立边 + 斜肩。openMm 是对面整体右移的量，
/// raiseMm 是对面整体抬高的量（面差）。withWall=false 模拟被遮挡的相机：看不到立边，
/// 也看不到圆边最后 0.3 mm。
PointCloud notchCloud(double openMm, bool withWall = true, double raiseMm = 0.0) {
  PointCloud c;
  const auto yA = [](double xMm) { return kNotchY0Mm + kNotchSlopeA * (xMm - kNotchXcMm); };
  for (int i = 0; i <= 100; ++i) {
    const double xMm = kNotchXcMm - 10.0 + 0.1 * i;
    const double edge = xMm > kNotchXcMm - 1.0 ? kNotchEdgeDrop * (xMm - (kNotchXcMm - 1.0)) : 0.0;
    if (!withWall && xMm > kNotchXcMm - 0.3) continue;
    c.push(notchMm(xMm), notchMm(yA(xMm) + edge), 0.0f);
  }
  const double yCorner = yA(kNotchXcMm) + kNotchEdgeDrop - raiseMm;
  const double xWall = kNotchXcMm + openMm;
  if (withWall) {
    for (int i = 0; i <= 12; ++i) {
      c.push(notchMm(xWall), notchMm(yCorner - 0.1 * i), 0.0f);
    }
  }
  for (int i = 1; i <= 80; ++i) {
    const double xMm = xWall + 0.1 * i;
    c.push(notchMm(xMm), notchMm(yCorner - kNotchWallHeight - 0.6 * 0.1 * i), 0.0f);
  }
  return c;
}

/// 在基准线下方 level 处，圆边穿出点离缝角 (1 - level/drop) mm，立边就在 xWall 上。
double notchNominalMm(double levelMm, double openMm) {
  return (1.0 - levelMm / kNotchEdgeDrop) + openMm;
}

}  // namespace

TEST_CASE("gap.flush 的 scale 与 offset 是线性的") {
  Line2D base;
  base.point[0] = 0.0f;
  base.point[1] = 0.0f;
  base.dir[0] = 1.0f;
  base.dir[1] = 0.0f;
  Point2D ref;
  ref.p[0] = 0.0f;
  ref.p[1] = 0.002f;  // 2 mm
  Call call;
  call.inputs["baseLine"] = Data::line2d(base);
  call.inputs["refPoint"] = Data::point2d(ref);
  REQUIRE(call.run("gap.flush").ok);
  CHECK(call.out("value").asMeasurement()->value == doctest::Approx(2.0).epsilon(1e-6));
  REQUIRE(call.run("gap.flush", {{"scale", Value::number(2.06)}, {"offset", Value::number(-0.1)}}).ok);
  CHECK(call.out("value").asMeasurement()->value ==
        doctest::Approx(2.0 * 2.06 - 0.1).epsilon(1e-6));
}

TEST_CASE("gap.flush 的 signed 模式给带符号垂距，两侧不再折回同一方向") {
  Line2D base;
  base.point[0] = 0.0f;
  base.point[1] = 0.0f;
  base.dir[0] = 1.0f;
  base.dir[1] = 0.0f;
  Call call;
  call.inputs["baseLine"] = Data::line2d(base);
  Point2D below;
  below.p[0] = 0.0f;
  below.p[1] = 0.002f;  // y 更大 = 基准线下方
  Point2D above;
  above.p[0] = 0.0f;
  above.p[1] = -0.002f;
  call.inputs["refPoint"] = Data::point2d(below);
  REQUIRE(call.run("gap.flush", {{"signed", Value::boolean(true)}}).ok);
  const double d1 = call.out("value").asMeasurement()->value;
  call.inputs["refPoint"] = Data::point2d(above);
  REQUIRE(call.run("gap.flush", {{"signed", Value::boolean(true)}}).ok);
  const double d2 = call.out("value").asMeasurement()->value;
  CHECK(d1 == doctest::Approx(-2.0).epsilon(1e-5));
  CHECK(d2 == doctest::Approx(2.0).epsilon(1e-5));
  // 默认仍是绝对值：两侧读数相同
  call.inputs["refPoint"] = Data::point2d(below);
  REQUIRE(call.run("gap.flush").ok);
  CHECK(call.out("value").asMeasurement()->value == doctest::Approx(2.0).epsilon(1e-5));
}

TEST_CASE("gap.notch_width 闭合缝读出圆边固有宽度，张开多少就多读多少") {
  for (const double open : {0.0, 0.5, 1.2}) {
    Call call;
    call.inputs["primary"] = Data::cloud(notchCloud(open));
    call.inputs["secondary"] = Data::cloud(notchCloud(open));
    REQUIRE(call.run("gap.notch_width").ok);
    CHECK(call.out("gap").asMeasurement()->value ==
          doctest::Approx(notchNominalMm(0.25, open)).epsilon(0.03));
    const Record* q = call.out("quality").asRecord();
    REQUIRE(q != nullptr);
    CHECK(q->type == "GapNotchQuality");
    CHECK(q->data["validCameras"].get<int>() == 2);
    CHECK(q->data["cameras"]["primary"]["cornerDepthMm"].get<double>() ==
          doctest::Approx(kNotchEdgeDrop).epsilon(0.05));
    const Point2D* anchor = call.out("anchor").asPoint2D();
    REQUIRE(anchor != nullptr);
    CHECK(std::fabs(static_cast<double>(anchor->p[0]) - kNotchXcMm / 1000.0) < 0.0002);
    CHECK(call.out("qualityBase").asRecord()->type == "GapFitQuality");
  }
}

TEST_CASE("gap.notch_width 换 levelDepth 沿圆边滑动，读数按 1/坡度 变") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
  REQUIRE(call.run("gap.notch_width", {{"levelDepth", Value::number(0.4)}}).ok);
  CHECK(call.out("gap").asMeasurement()->value ==
        doctest::Approx(notchNominalMm(0.4, 0.0)).epsilon(0.05));
}

TEST_CASE("gap.notch_width 的 camera 只取指定那一台，both 取有效相机的平均") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.6));
  REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("secondary")}}).ok);
  CHECK(call.out("gap").asMeasurement()->value ==
        doctest::Approx(notchNominalMm(0.25, 0.6)).epsilon(0.03));
  REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("primary")}}).ok);
  CHECK(call.out("gap").asMeasurement()->value ==
        doctest::Approx(notchNominalMm(0.25, 0.0)).epsilon(0.03));
  REQUIRE(call.run("gap.notch_width").ok);
  CHECK(call.out("gap").asMeasurement()->value ==
        doctest::Approx(notchNominalMm(0.25, 0.3)).epsilon(0.03));
}

TEST_CASE("gap.notch_width 被遮挡的相机量的是自己的阴影，与看得见立边的那台不同") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.0, /*withWall=*/false));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
  REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("secondary")}}).ok);
  const double seen = call.out("gap").asMeasurement()->value;
  CHECK(seen == doctest::Approx(notchNominalMm(0.25, 0.0)).epsilon(0.03));
  REQUIRE(call.run("gap.notch_width", {{"camera", Value::text("primary")}}).ok);
  const double shadow = call.out("gap").asMeasurement()->value;
  CHECK(std::fabs(shadow - seen) > 0.3);
}

TEST_CASE("gap.notch_width 对面整体抬高（面差）不改变开口读数") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.0));
  REQUIRE(call.run("gap.notch_width").ok);
  const double flat = call.out("gap").asMeasurement()->value;
  call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.3));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.3));
  REQUIRE(call.run("gap.notch_width").ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(flat).epsilon(0.02));
  const Record* q = call.out("quality").asRecord();
  REQUIRE(q != nullptr);
  CHECK(q->data["cameras"]["secondary"]["levelYMm"].is_number());
}

TEST_CASE("gap.notch_width 没有 V 缝就报 notch_too_shallow") {
  PointCloud flat;
  for (int i = 0; i <= 200; ++i) {
    const double xMm = kNotchXcMm - 10.0 + 0.1 * i;
    flat.push(notchMm(xMm), notchMm(kNotchY0Mm + kNotchSlopeA * (xMm - kNotchXcMm)), 0.0f);
  }
  Call call;
  call.inputs["primary"] = Data::cloud(flat);
  call.inputs["secondary"] = Data::cloud(flat);
  const Status s = call.run("gap.notch_width");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "notch_too_shallow");
}

TEST_CASE("gap.notch_width 的 scale 与 gapOffset 是线性的") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
  REQUIRE(call.run("gap.notch_width").ok);
  const double raw = call.out("gap").asMeasurement()->value;
  REQUIRE(call.run("gap.notch_width",
                   {{"scale", Value::number(2.0)}, {"gapOffset", Value::number(-0.1)}})
              .ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(raw * 2.0 - 0.1).epsilon(1e-6));
}

TEST_CASE("gap.notch_width 的面差随对面抬高而变，flushBase 决定符号") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.0));
  REQUIRE(call.run("gap.notch_width").ok);
  REQUIRE(call.out("flush").asMeasurement()->ok);
  const double flat = call.out("flush").asMeasurement()->value;
  // 对面（右侧，斜肩 -0.6）整体抬高 0.3：以左为基准，右侧更高 → 面差增加约 0.3·cos(atan 0.4)
  call.inputs["primary"] = Data::cloud(notchCloud(0.4, true, 0.3));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.4, true, 0.3));
  REQUIRE(call.run("gap.notch_width").ok);
  const double raised = call.out("flush").asMeasurement()->value;
  CHECK(raised - flat == doctest::Approx(0.3 * std::cos(std::atan(kNotchSlopeA))).epsilon(0.05));
  CHECK(call.out("refLine").asLine2D() != nullptr);
  CHECK(call.out("flushSegment").asLine2D() != nullptr);
  CHECK(call.out("qualityRef").asRecord()->type == "GapFitQuality");
  // 换面差基准到右侧：符号取反；垂距按新基准线的方向算，两侧坡度不同（0.4 与 -0.6）所以大小差几个百分点
  REQUIRE(call.run("gap.notch_width", {{"flushBase", Value::text("right")}}).ok);
  CHECK(call.out("flush").asMeasurement()->value == doctest::Approx(-raised).epsilon(0.1));
  // 开口不受 flushBase 影响
  const double gapRight = call.out("gap").asMeasurement()->value;
  REQUIRE(call.run("gap.notch_width", {{"flushBase", Value::text("left")}}).ok);
  CHECK(call.out("gap").asMeasurement()->value == doctest::Approx(gapRight).epsilon(1e-6));
}

TEST_CASE("gap.notch_width 对面翼面拟不出来时面差无效、开口照常") {
  Call call;
  call.inputs["primary"] = Data::cloud(notchCloud(0.0));
  call.inputs["secondary"] = Data::cloud(notchCloud(0.0));
  REQUIRE(call.run("gap.notch_width", {{"refNear", Value::number(20.0)}, {"refFar", Value::number(25.0)}}).ok);
  CHECK(call.out("gap").asMeasurement()->ok);
  CHECK_FALSE(call.out("flush").asMeasurement()->ok);
  // 声明过的输出端口一个都不能少，否则执行器报 output_not_written（点 3 的合成位移上踩到过）。
  CHECK(call.out("refLine").asLine2D() != nullptr);
  CHECK(call.out("flushSegment").asLine2D() != nullptr);
  CHECK(call.out("qualityRef").asRecord() != nullptr);
}

TEST_CASE("gap.result_bundle 不接 flush 时把它记成 inactive") {
  Call call;
  Measurement gap;
  gap.value = 0.5;
  gap.ok = true;
  call.inputs["gap"] = Data::measurement(gap);
  REQUIRE(call.run("gap.result_bundle").ok);
  const Record* bundle = call.out("bundle").asRecord();
  REQUIRE(bundle != nullptr);
  CHECK(bundle->data["flush"]["status"].get<std::string>() == "inactive");
  CHECK(bundle->data["gap"]["status"].get<std::string>() == "success");
}


namespace {

struct Uh {
  float u;
  float h;
};

const Uh kProfileSamples[4] = {{0.001f, 0.010f}, {0.002f, 0.012f}, {0.003f, 0.009f},
                               {0.004f, 0.011f}};

void writeAsciiPcd(const std::filesystem::path& file, bool profileLayout) {
  std::ofstream out(file);
  out << "# .PCD v0.7 - Point Cloud Data file format\n"
      << "VERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F F\nCOUNT 1 1 1 1\n"
      << "WIDTH 4\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 4\nDATA ascii\n";
  for (const Uh& s : kProfileSamples) {
    const float y = profileLayout ? s.h : 0.0f;
    const float z = profileLayout ? 0.0f : s.h;
    out << s.u << " " << y << " " << z << " 0\n";
  }
}

}  // namespace

TEST_CASE("gap.load_profile_pair 的 layout=profile 读出来与 sensor 布局逐点一致") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "lyflow-gap-layout-test";
  std::filesystem::create_directories(dir);
  const std::filesystem::path sensorPcd = dir / "sensor.pcd";
  const std::filesystem::path profilePcd = dir / "profile.pcd";
  writeAsciiPcd(sensorPcd, /*profileLayout=*/false);
  writeAsciiPcd(profilePcd, /*profileLayout=*/true);

  const auto load = [](const std::filesystem::path& file, const char* layout) {
    Call call;
    REQUIRE(call.run("gap.load_profile_pair",
                     {
                         {"source", Value::text("files")},
                         {"primaryFile", Value::text(file.string())},
                         {"secondaryFile", Value::text(file.string())},
                         {"layout", Value::text(layout)},
                     })
                .ok);
    return call;
  };

  Call sensor = load(sensorPcd, "sensor");
  Call profile = load(profilePcd, "profile");

  for (const char* port : {"primary", "secondary"}) {
    CAPTURE(port);
    const PointCloud* a = sensor.out(port).asCloud();
    const PointCloud* b = profile.out(port).asCloud();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->pointCount() == 4);
    REQUIRE(b->pointCount() == a->pointCount());
    for (std::size_t i = 0; i < a->pointCount(); ++i) {
      CAPTURE(i);
      CHECK(b->xyz[i * 3] == doctest::Approx(a->xyz[i * 3]));
      CHECK(b->xyz[i * 3 + 1] == doctest::Approx(a->xyz[i * 3 + 1]));
      CHECK(b->xyz[i * 3 + 2] == doctest::Approx(a->xyz[i * 3 + 2]));
    }
    CHECK(a->xyz[1] == doctest::Approx(0.0f));
    CHECK(b->xyz[2] == doctest::Approx(kProfileSamples[0].h));
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
