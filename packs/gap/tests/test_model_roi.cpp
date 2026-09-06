// 模型 ROI 路径的单测（计划第二部分 §9/§10）。不碰 ONNX：推理那一步在
// gap_ml 自己的金标准测试里已经逐值锁死，这里测的是「包把它接进 LyFlow」的那部分。
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

constexpr std::size_t kSlots = 1280;

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

/// 毫米四元组 → Box2D（米）。测试里给的都是毫米，与算子参数同口径。
Box2D boxMm(double xMin, double yMin, double xMax, double yMax) {
  return box(static_cast<float>(xMin) / 1000.0F, static_cast<float>(yMin) / 1000.0F,
             static_cast<float>(xMax) / 1000.0F, static_cast<float>(yMax) / 1000.0F);
}

/// n 个点排在 y=0.170 m 一线上，x 从 0 起每毫米一个。
PointCloud rowCloud(std::size_t n, float x0Mm, float yMm) {
  PointCloud c;
  for (std::size_t i = 0; i < n; ++i) {
    c.push((x0Mm + static_cast<float>(i)) / 1000.0F, yMm / 1000.0F, 0.0F);
  }
  return c;
}

/// 1280 槽的传感器帧剖面：x 从 x0Mm 起步长 stepMm，z = zMm。
PointCloud profile(double x0Mm, double stepMm, double zMm) {
  PointCloud c;
  c.rgb.reserve(kSlots * 3);
  for (std::size_t i = 0; i < kSlots; ++i) {
    c.push(static_cast<float>((x0Mm + stepMm * static_cast<double>(i)) / 1000.0),
           0.0F, static_cast<float>(zMm / 1000.0));
    c.rgb.push_back(120);
    c.rgb.push_back(120);
    c.rgb.push_back(120);
  }
  return c;
}

/// 一行标签：把 [from, to) 这些槽标成 cls，其余是 0（背景）。
void paint(std::vector<int>& labels, std::size_t from, std::size_t to, int cls) {
  for (std::size_t i = from; i < to && i < labels.size(); ++i) labels[i] = cls;
}

Record labelsRecord(const std::vector<int>& row0, const std::vector<int>& row1) {
  Record r;
  r.type = "GapLabels";
  r.data["row0"] = row0;
  r.data["row1"] = row1;
  return r;
}

/// roll_anchored_crop 跑一次，回状态字符串。
std::string cropStatus(Call& call) {
  const Record* record = call.out("status").asRecord();
  REQUIRE(record != nullptr);
  return record->data["status"].get<std::string>();
}

}  // namespace

TEST_CASE("gap.roll_anchored_crop 的五种 skip_reason") {
  // 一对合理的 roll 框：x 跨度 10 mm（在 [2, 40] 内），各自 z 高 2 mm。
  const Box2D goodLeft = boxMm(0.0, 168.0, 3.0, 170.0);
  const Box2D goodRight = boxMm(7.0, 168.0, 10.0, 170.0);

  struct Case {
    const char* name;
    const char* want;
    Box2D left;
    Box2D right;
    std::unordered_map<std::string, Value> params;
  };
  const Case cases[5] = {
      {"关掉", "disabled", goodLeft, goodRight, {{"enabled", Value::boolean(false)}}},
      // 半宽为 0 —— 配置本身不可用，先于任何标签判掉
      {"坏配置", "rejected:bad_config", goodLeft, goodRight, {{"halfWidth", Value::number(0.0)}}},
      // hi <= lo：一个塌掉的框不带任何锚点信息
      {"退化框", "rejected:degenerate_roll_box", boxMm(3.0, 168.0, 3.0, 170.0), goodRight, {}},
      // 单个 roll 框比 maxRollBoxHeight 还高 —— 标签本身就不可信
      {"框太高", "rejected:roll_box_height", boxMm(0.0, 160.0, 3.0, 180.0), goodRight, {}},
      // 两框合起来的 x 跨度 60 mm > 40 mm
      {"跨度不合理", "rejected:span", goodLeft, boxMm(57.0, 168.0, 60.0, 170.0), {}},
  };

  for (const Case& c : cases) {
    CAPTURE(c.name);
    Call call;
    call.inputs["primary"] = Data::cloud(rowCloud(200, 0.0F, 169.0F));
    call.inputs["secondary"] = Data::cloud(rowCloud(200, 0.0F, 169.0F));
    call.inputs["gapLeft"] = Data::box2d(c.left);
    call.inputs["gapRight"] = Data::box2d(c.right);
    REQUIRE(call.run("gap.roll_anchored_crop", c.params).ok);
    CHECK(cropStatus(call) == c.want);
    // 没生效就必须原样透传
    CHECK(call.out("primary").asCloud()->pointCount() == 200);
    CHECK(call.out("secondary").asCloud()->pointCount() == 200);
  }
}

TEST_CASE("gap.roll_anchored_crop 生效时按窗裁，点数不够就回退") {
  // 窗中心 = 两 roll 框中心的中点 = (5, 169) mm，半宽 4 / 半高 3 -> [1, 166, 9, 172] mm。
  // 云在 y=169 mm 上、x = 0..19 mm，严格开区间 -> 留下 x = 2..8 共 7 个点。
  const Box2D left = boxMm(0.0, 168.0, 3.0, 170.0);
  const Box2D right = boxMm(7.0, 168.0, 10.0, 170.0);
  const std::unordered_map<std::string, Value> window{
      {"halfWidth", Value::number(4.0)}, {"halfHeight", Value::number(3.0)}};

  Call applied;
  applied.inputs["primary"] = Data::cloud(rowCloud(20, 0.0F, 169.0F));
  applied.inputs["secondary"] = Data::cloud(rowCloud(20, 0.0F, 169.0F));
  applied.inputs["gapLeft"] = Data::box2d(left);
  applied.inputs["gapRight"] = Data::box2d(right);
  auto params = window;
  params["minPointsKept"] = Value::integer(10);  // 7 + 7 = 14 >= 10
  REQUIRE(applied.run("gap.roll_anchored_crop", params).ok);
  CHECK(cropStatus(applied) == "applied");
  CHECK(applied.out("primary").asCloud()->pointCount() == 7);
  CHECK(applied.out("secondary").asCloud()->pointCount() == 7);
  const Box2D* win = applied.out("window").asBox2D();
  REQUIRE(win != nullptr);
  CHECK(win->min[0] == doctest::Approx(0.001));
  CHECK(win->max[0] == doctest::Approx(0.009));

  Call reverted;
  reverted.inputs["primary"] = Data::cloud(rowCloud(20, 0.0F, 169.0F));
  reverted.inputs["secondary"] = Data::cloud(rowCloud(20, 0.0F, 169.0F));
  reverted.inputs["gapLeft"] = Data::box2d(left);
  reverted.inputs["gapRight"] = Data::box2d(right);
  params["minPointsKept"] = Value::integer(15);  // 14 < 15 -> 丢掉窗
  REQUIRE(reverted.run("gap.roll_anchored_crop", params).ok);
  CHECK(cropStatus(reverted) == "reverted:min_points");
  CHECK(reverted.out("primary").asCloud()->pointCount() == 20);
  CHECK(reverted.out("secondary").asCloud()->pointCount() == 20);
  // 回退之后 window 退成剩余点的包围盒，而不是那个 1e7 的无界框
  const Box2D* fallback = reverted.out("window").asBox2D();
  REQUIRE(fallback != nullptr);
  CHECK(fallback->min[0] == doctest::Approx(0.0));
  CHECK(fallback->max[0] == doctest::Approx(0.019));
}

TEST_CASE("gap.roll_anchored_crop 的点数保护按 usingCamera 分") {
  // primary 在窗里有 7 个点，secondary 一个都没有（y 差了 20 mm）。
  auto make = [](const char* camera, std::int64_t minimum) {
    Call call;
    call.inputs["primary"] = Data::cloud(rowCloud(20, 0.0F, 169.0F));
    call.inputs["secondary"] = Data::cloud(rowCloud(20, 0.0F, 189.0F));
    call.inputs["gapLeft"] = Data::box2d(boxMm(0.0, 168.0, 3.0, 170.0));
    call.inputs["gapRight"] = Data::box2d(boxMm(7.0, 168.0, 10.0, 170.0));
    REQUIRE(call.run("gap.roll_anchored_crop",
                     {{"halfWidth", Value::number(4.0)},
                      {"halfHeight", Value::number(3.0)},
                      {"minPointsKept", Value::integer(minimum)},
                      {"usingCamera", Value::text(camera)}})
                .ok);
    return call;
  };
  // Both 看两片之和：7 + 0 = 7 >= 7
  Call both = make("Both", 7);
  CHECK(cropStatus(both) == "applied");
  // Right 只看 secondary：0 < 7
  Call right = make("Right", 7);
  CHECK(cropStatus(right) == "reverted:min_points");
  // Left 只看 primary：7 >= 7
  Call left = make("Left", 7);
  CHECK(cropStatus(left) == "applied");
}

TEST_CASE("gap.roi_from_labels 把框换成测量帧的米，段缺失时报 model_roi_failed") {
  // 两片剖面：x 每槽 0.1 mm，z 分别在 168 / 175 mm。标签只标 row0。
  PointCloud primary = profile(-64.0, 0.1, 168.0);
  PointCloud secondary = profile(-64.0, 0.1, 175.0);
  std::vector<int> row0(kSlots, 0);
  std::vector<int> row1(kSlots, 0);
  // 类 id：2=left_surface 3=left_roll 5=right_roll 6=right_surface（契约固定）
  paint(row0, 100, 200, 2);
  paint(row0, 300, 340, 3);
  paint(row0, 400, 440, 5);
  paint(row0, 600, 700, 6);

  Call call;
  call.inputs["primary"] = Data::cloud(primary);
  call.inputs["secondary"] = Data::cloud(secondary);
  call.inputs["labels"] = Data::record(labelsRecord(row0, row1));
  REQUIRE(call.run("gap.roi_from_labels", {{"refine", Value::boolean(false)}}).ok);

  // left_surface -> flushBase：槽 100..199 -> x = -54.0 .. -44.1 mm，z 恒 168 mm
  const Box2D* base = call.out("flushBase").asBox2D();
  REQUIRE(base != nullptr);
  CHECK(base->min[0] == doctest::Approx(-0.054).epsilon(1e-4));
  CHECK(base->max[0] == doctest::Approx(-0.0441).epsilon(1e-4));
  CHECK(base->min[1] == doctest::Approx(0.168).epsilon(1e-5));
  CHECK(base->max[1] == doctest::Approx(0.168).epsilon(1e-5));
  // left_roll -> gapLeft，right_roll -> gapRight，right_surface -> flushRef
  CHECK(call.out("gapLeft").asBox2D()->min[0] == doctest::Approx(-0.034).epsilon(1e-4));
  CHECK(call.out("gapRight").asBox2D()->min[0] == doctest::Approx(-0.024).epsilon(1e-4));
  CHECK(call.out("flushRef").asBox2D()->min[0] == doctest::Approx(-0.004).epsilon(1e-3));

  // 少标一段：missing_segments 非空 -> model_roi_failed，消息里点名缺的那段
  std::vector<int> missing(kSlots, 0);
  paint(missing, 100, 200, 2);
  paint(missing, 300, 340, 3);
  paint(missing, 400, 440, 5);
  Call bad;
  bad.inputs["primary"] = Data::cloud(primary);
  bad.inputs["secondary"] = Data::cloud(secondary);
  bad.inputs["labels"] = Data::record(labelsRecord(missing, row1));
  const Status s = bad.run("gap.roi_from_labels", {{"refine", Value::boolean(false)}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "model_roi_failed");
  CHECK(s.message.find("right_surface") != std::string::npos);
  // 诊断照样出来，图上点开那个节点看得到
  REQUIRE(bad.out("refinements").asRecord() != nullptr);
}

TEST_CASE("gap.roi_from_labels 的输入点数必须是 1280") {
  std::vector<int> row(kSlots, 0);
  paint(row, 100, 200, 2);
  Call call;
  call.inputs["primary"] = Data::cloud(rowCloud(10, 0.0F, 168.0F));
  call.inputs["secondary"] = Data::cloud(profile(-64.0, 0.1, 175.0));
  call.inputs["labels"] = Data::record(labelsRecord(row, row));
  const Status s = call.run("gap.roi_from_labels");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "primary");
}

TEST_CASE("gap.roi_from_labels 的 backdrop 零拷贝透传，没接就是空云") {
  std::vector<int> row0(kSlots, 0);
  std::vector<int> row1(kSlots, 0);
  paint(row0, 100, 200, 2);
  paint(row0, 300, 340, 3);
  paint(row0, 400, 440, 5);
  paint(row0, 600, 700, 6);
  auto feed = [&](Call& call) {
    call.inputs["primary"] = Data::cloud(profile(-64.0, 0.1, 168.0));
    call.inputs["secondary"] = Data::cloud(profile(-64.0, 0.1, 175.0));
    call.inputs["labels"] = Data::record(labelsRecord(row0, row1));
  };

  Call linked;
  feed(linked);
  const Data backdrop = Data::cloud(profile(-64.0, 0.1, 168.0));
  linked.inputs["backdrop"] = backdrop;
  REQUIRE(linked.run("gap.roi_from_labels", {{"refine", Value::boolean(false)}}).ok);
  // 同一个 shared_ptr 出去，不是复制一份
  CHECK(linked.out("backdrop").cloudPtr() == backdrop.cloudPtr());
  CHECK(linked.out("backdrop").asCloud()->pointCount() == kSlots);

  Call bare;
  feed(bare);
  REQUIRE(bare.run("gap.roi_from_labels", {{"refine", Value::boolean(false)}}).ok);
  REQUIRE(bare.out("backdrop").asCloud() != nullptr);
  CHECK(bare.out("backdrop").asCloud()->pointCount() == 0);
}

TEST_CASE("gap.labels_to_cloud 按类上色") {
  std::vector<int> row0(kSlots, 0);
  paint(row0, 0, 100, 2);    // left_surface
  paint(row0, 100, 200, 3);  // left_roll
  std::vector<int> row1(kSlots, 5);

  Call call;
  call.inputs["cloud"] = Data::cloud(profile(-64.0, 0.1, 168.0));
  call.inputs["labels"] = Data::record(labelsRecord(row0, row1));
  REQUIRE(call.run("gap.labels_to_cloud").ok);
  const PointCloud* out = call.out("cloud").asCloud();
  REQUIRE(out != nullptr);
  REQUIRE(out->hasRgb());
  REQUIRE(out->pointCount() == kSlots);
  // 三段各取一个点：背景、left_surface、left_roll 必须是三种不同的颜色
  auto colorAt = [&](std::size_t i) {
    return std::array<std::uint8_t, 3>{out->rgb[i * 3], out->rgb[i * 3 + 1], out->rgb[i * 3 + 2]};
  };
  CHECK(colorAt(50) == std::array<std::uint8_t, 3>{60, 170, 255});
  CHECK(colorAt(150) == std::array<std::uint8_t, 3>{255, 190, 40});
  CHECK(colorAt(500) == std::array<std::uint8_t, 3>{60, 60, 60});
  // 类 id 同时进 intensity —— 3D 视图没有 rgb 着色模式，靠它才看得见
  REQUIRE(out->hasIntensity());
  CHECK(out->intensity[50] == doctest::Approx(2.0));
  CHECK(out->intensity[150] == doctest::Approx(3.0));
  CHECK(out->intensity[500] == doctest::Approx(0.0));

  // row 参数换成 secondary，取的就是 row1（整行都是 right_roll）
  Call other;
  other.inputs["cloud"] = Data::cloud(profile(-64.0, 0.1, 168.0));
  other.inputs["labels"] = Data::record(labelsRecord(row0, row1));
  REQUIRE(other.run("gap.labels_to_cloud", {{"row", Value::text("secondary")}}).ok);
  const PointCloud* second = other.out("cloud").asCloud();
  CHECK(second->rgb[150 * 3] == 255);
  CHECK(second->rgb[150 * 3 + 1] == 90);
  CHECK(second->rgb[150 * 3 + 2] == 60);
}

TEST_CASE("gap.drop_non_finite 只丢非有限点，其余通道跟着搬") {
  PointCloud c;
  c.push(0.0F, 0.0F, 0.0F);
  c.push(std::nanf(""), 1.0F, 0.0F);
  c.push(2.0F, 2.0F, 0.0F);
  c.push(3.0F, std::numeric_limits<float>::infinity(), 0.0F);
  c.rgb = {1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4};

  Call call;
  call.inputs["cloud"] = Data::cloud(std::move(c));
  REQUIRE(call.run("gap.drop_non_finite").ok);
  const PointCloud* out = call.out("cloud").asCloud();
  REQUIRE(out != nullptr);
  CHECK(out->pointCount() == 2);
  CHECK(out->xyz[0] == doctest::Approx(0.0));
  CHECK(out->xyz[3] == doctest::Approx(2.0));
  REQUIRE(out->hasRgb());
  CHECK(out->rgb[0] == 1);
  CHECK(out->rgb[3] == 3);
}

// -------------------------------------------------- ONNX 三步的头尾两步（T6）

TEST_CASE("gap.profile_tensor 出 [2, 6, 1280]，valid 通道认得出 NaN 槽") {
  PointCloud primary = profile(-30.0, 0.05, 100.0);
  PointCloud secondary = profile(-30.0, 0.05, 101.0);
  primary.xyz[0] = std::nanf("");  // 第 0 槽无效

  Call call;
  call.inputs["primary"] = Data::cloud(primary);
  call.inputs["secondary"] = Data::cloud(secondary);
  const Status s = call.run("gap.profile_tensor");
  CAPTURE(s.message);
  REQUIRE(s.ok);

  const Tensor* t = call.out("tensor").asTensor();
  REQUIRE(t != nullptr);
  CHECK(t->shape == std::vector<std::int64_t>{2, 6, 1280});
  CHECK(t->consistent());
  // 通道 5 是 valid：primary 的第 0 槽是 0，第 1 槽是 1
  CHECK(t->data[5 * kSlots + 0] == 0.0F);
  CHECK(t->data[5 * kSlots + 1] == 1.0F);
}

TEST_CASE("gap.profile_tensor 点数不是 1280 时报 bad_input 并指出端口") {
  Call call;
  call.inputs["primary"] = Data::cloud(rowCloud(10, 0.0F, 0.0F));
  call.inputs["secondary"] = Data::cloud(profile(-30.0, 0.05, 101.0));
  const Status s = call.run("gap.profile_tensor");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "primary");
}

TEST_CASE("gap.labels_from_logits 逐槽 argmax，并列时留最小的类 id") {
  Tensor logits;
  const std::int64_t classes = 8;
  logits.shape = {2, classes, static_cast<std::int64_t>(kSlots)};
  logits.data.assign(2 * classes * kSlots, 0.0F);
  // row0 的第 7 槽让类 3 赢；row1 的第 9 槽让类 5 赢；其余全 0 -> 并列取 0
  logits.data[3 * kSlots + 7] = 1.0F;
  logits.data[classes * kSlots + 5 * kSlots + 9] = 2.0F;

  Call call;
  call.inputs["tensor"] = Data::tensor(logits);
  const Status s = call.run("gap.labels_from_logits");
  CAPTURE(s.message);
  REQUIRE(s.ok);

  const Record* record = call.out("labels").asRecord();
  REQUIRE(record != nullptr);
  CHECK(record->type == "GapLabels");
  CHECK(record->data["row0"].size() == kSlots);
  CHECK(record->data["row0"][7].get<int>() == 3);
  CHECK(record->data["row0"][6].get<int>() == 0);
  CHECK(record->data["row1"][9].get<int>() == 5);
}

TEST_CASE("gap.labels_from_logits 形状不对时报 bad_input") {
  Tensor bad;
  bad.shape = {2, 8, 16};
  bad.data.assign(2 * 8 * 16, 0.0F);
  Call call;
  call.inputs["tensor"] = Data::tensor(bad);
  const Status s = call.run("gap.labels_from_logits");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "tensor");
}
