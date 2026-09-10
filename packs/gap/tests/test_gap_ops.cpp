// gap 算子包的单测。跟 LyFlow core 共用同一个 doctest 目标（ADR-0013）。
// 这里只测「必须逐条复刻」的那几条语义（计划 §3），拟合本身是复用的库函数，不重测。
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
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
