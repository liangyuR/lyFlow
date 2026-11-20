// dts 算子包的单测。跟 LyFlow core 共用同一个 doctest 目标（ADR-0013）。
// 手法与 packs/gap/tests/test_gap_ops.cpp 相同：只注册本包的 Registry，用 test::OpCall（core/tests/helpers.h）
// 铺 manifest 默认值再 override 直接调 compute —— 不经执行器，一步出错就停。
//
// 这里测的是「整条链在实测轮廓上跑得通、失败时错在该错的那一步」，
// 不重测拟合本身。与宿主 Python 旧算法的数值对照见 `现场样本` 那几个用例：
// 只有差得住的样本写死容差，差得多的在 README 的「已知差距」里记着。
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/field_profiles.h"
#include "data/real_profiles.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/profile.h"
#include <numeric>
#include <fstream>
#include "ops/dts_ops.h"

namespace lyflow::packs::std_profile {
void registerPackOps(Registry&);
}

namespace lyflow::packs::solution_dts {
void registerPackOps(Registry& r);
}

namespace {

using namespace lyflow;
namespace td = lyflow::dts::testdata;

/// 不取消、不记进度的最小 ExecContext。
const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::std_profile::registerPackOps(reg);
    packs::solution_dts::registerPackOps(reg);
    return reg;
  }();
  return r;
}

/// 直接调一个算子的 compute：参数先铺默认值，再用 overrides 覆盖。
/// 直接调 compute（test::OpCall），只用本包的注册表。
struct Call : test::OpCall {
  Call() : OpCall(packRegistry()) {}
};

/// default-profile.lyflow.json 那条链，一步出错就停在那里，
/// `failedOp` 记住是哪一步 —— 失败路径的用例断言的就是它。
Data nativeCloud(PointCloud cloud) {
  ProfileDescriptor descriptor;
  descriptor.frame = "dts-scan";
  descriptor.unit = "mm";
  descriptor.plane = "XZ";
  descriptor.handedness = "right";
  descriptor.sampleOrder = "acquisition";
  descriptor.sourceId = "dts-source";
  descriptor.sourceCount = cloud.pointCount();
  std::vector<std::uint8_t> validity(cloud.pointCount());
  std::vector<std::uint32_t> sources(cloud.pointCount());
  std::iota(sources.begin(), sources.end(), 0);
  for (std::size_t i = 0; i < cloud.pointCount(); ++i)
    validity[i] = std::isfinite(cloud.xyz[3 * i]) && std::isfinite(cloud.xyz[3 * i + 1]) &&
                  std::isfinite(cloud.xyz[3 * i + 2]);
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)), descriptor,
                      std::move(validity), std::move(sources), profile, why));
  return Data::profile(std::move(profile));
}

struct Chain {
  Call clean, faces, dome, metal, root, flush, bundle;
  std::unordered_map<std::string, Value> cleanOverrides, metalOverrides;
  std::string failedOp;
  Status status;

  bool run(PointCloud raw) { return runProfile(nativeCloud(std::move(raw))); }

  bool runProfile(Data source) {
    clean.inputs["profile"] = std::move(source);
    if (!step(clean, "profile.clean", cleanOverrides))
      return false;

    faces.inputs["clean"] = clean.out("clean");
    if (!step(faces, "profile.split_faces"))
      return false;

    dome.inputs["clean"] = clean.out("clean");
    dome.inputs["faces"] = faces.out("faces");
    if (!step(dome, "dts.seal_dome"))
      return false;

    metal.inputs["clean"] = clean.out("clean");
    metal.inputs["faces"] = faces.out("faces");
    metal.inputs["dome"] = dome.out("dome");
    if (!step(metal, "dts.pick_metal", metalOverrides))
      return false;

    root.inputs["clean"] = clean.out("clean");
    root.inputs["faces"] = faces.out("faces");
    root.inputs["dome"] = dome.out("dome");
    root.inputs["pick"] = metal.out("pick");
    if (!step(root, "dts.seal_root"))
      return false;

    flush.inputs["root"] = root.out("root");
    if (!step(flush, "dts.flush"))
      return false;

    bundle.inputs["faces"] = faces.out("faces");
    bundle.inputs["dome"] = dome.out("dome");
    bundle.inputs["pick"] = metal.out("pick");
    bundle.inputs["root"] = root.out("root");
    bundle.inputs["flush"] = flush.out("flush");
    bundle.inputs["footInfo"] = flush.out("footInfo");
    return step(bundle, "dts.profile_bundle");
  }

private:
  bool step(Call& c, const char* id, const std::unordered_map<std::string, Value>& overrides = {}) {
    status = c.run(id, overrides);
    if (!status.ok)
      failedOp = id;
    return status.ok;
  }
};

/// x/z 两个数组 → PointCloud。注入契约：y 恒 0，亮度有效点 1.0。
PointCloud cloudOf(const float* x, const float* z, int n) {
  PointCloud c;
  c.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    c.push(x[i], 0.0f, z[i]);
  c.intensity.assign(static_cast<std::size_t>(n), 1.0f);
  return c;
}

/// real_profiles.h 那两条：x 由 x0 + k·dx 还原（等间距重采样过），z 照抄。
/// intensity 给 nullptr 就一律 1.0，给 kI* 就照抄原始亮度（0..255，0 是无效点）。
PointCloud resampled(const float* z, const unsigned char* intensity) {
  PointCloud c;
  c.reserve(static_cast<std::size_t>(td::kPoints));
  c.intensity.resize(static_cast<std::size_t>(td::kPoints));
  for (int i = 0; i < td::kPoints; ++i) {
    c.push(td::kX0Mm + static_cast<float>(i) * td::kDxMm, 0.0f, z[i]);
    c.intensity[static_cast<std::size_t>(i)] =
        intensity == nullptr ? 1.0f : static_cast<float>(intensity[i]);
  }
  return c;
}

std::size_t cleanCount(Chain& c) {
  const auto& valid = c.clean.out("clean").asProfile()->validity;
  return std::count(valid.begin(), valid.end(), std::uint8_t{1});
}

} // namespace

TEST_CASE("dts 整条链在两条实测轮廓上跑到底") {
  struct Case {
    const char* name;
    const float* z;
  };
  const Case cases[2] = {{"kZ1000", td::kZ1000}, {"kZ3000", td::kZ3000}};
  for (const Case& c : cases) {
    // doctest 对 const char* 打的是指针，所以一律先包成 std::string 再进 CAPTURE / MESSAGE。
    const std::string name(c.name);
    CAPTURE(name);
    Chain chain;
    REQUIRE_MESSAGE(chain.run(resampled(c.z, nullptr)),
                    chain.failedOp << ": " << chain.status.message);

    // 清理只会丢点，不会造点；丢光了 profile_clean 自己就报错了，所以这里必 > 0。
    const std::size_t kept = cleanCount(chain);
    CHECK(kept > 0);
    CHECK(kept <= static_cast<std::size_t>(td::kPoints));

    const Record* faces = chain.faces.out("faces").asRecord();
    REQUIRE(faces != nullptr);
    CHECK(faces->type == "profile.Faces.v2");
    CHECK(faces->data["faces"].size() >= 1);

    const Record* dome = chain.dome.out("dome").asRecord();
    REQUIRE(dome != nullptr);
    CHECK(dome->type == "DtsDome.v3");
    // 半径是胶条的硬判据：seal_dome 成功就意味着它落在 [7, 14] 里。
    CHECK(dome->data["radiusMm"].get<double>() > 0.0);

    const Line2D* metal = chain.metal.out("metal").asLine2D();
    REQUIRE(metal != nullptr);
    CHECK(metal->hasSegment);

    const Record* root = chain.root.out("root").asRecord();
    REQUIRE(root != nullptr);
    CHECK(root->type == "DtsRoot.v3");
    CHECK(root->data.contains("x"));
    CHECK(root->data.contains("z"));

    const Measurement* m = chain.flush.out("flush").asMeasurement();
    REQUIRE(m != nullptr);
    CHECK(m->ok);
    CHECK(std::isfinite(m->value));
    CHECK(m->unit == "mm");
    // 判定不在图里（dts.flush 的 doc），所以 verdict 必须是空的。
    CHECK(m->verdict.empty());
    MESSAGE(name << " 面差 = " << m->value << " mm，面 " << faces->data["faces"].size()
                 << " 块，清理后 " << kept << " 点");

    const Record* bundle = chain.bundle.out("bundle").asRecord();
    REQUIRE(bundle != nullptr);
    CHECK(bundle->type == "DtsProfileBundle.v3");
    CHECK(bundle->data["ok"].get<bool>());
    CHECK(bundle->data["flushMm"].get<double>() == doctest::Approx(m->value));
  }
}

TEST_CASE("profile_clean 的 minIntensity 按亮度裁点") {
  // kI1000 是原始亮度（0..255），0 是 DP2240 的无效点。默认门限 0.5 只砍掉那 155 个；
  // 门限提到 140 就连低亮度的点一起砍 —— 这条是为了证明门限真的在起作用，
  // 不是「反正 z 都有限所以一个不丢」。
  Call plain;
  plain.inputs["profile"] = nativeCloud(resampled(td::kZ1000, nullptr));
  REQUIRE(plain.run("profile.clean").ok);
  const auto& plainMask = plain.out("clean").asProfile()->validity;
  const std::size_t all = std::count(plainMask.begin(), plainMask.end(), std::uint8_t{1});
  CHECK(all == static_cast<std::size_t>(td::kPoints));

  Call defaulted;
  defaulted.inputs["profile"] = nativeCloud(resampled(td::kZ1000, td::kI1000));
  REQUIRE(defaulted.run("profile.clean").ok);
  const auto& keptMask = defaulted.out("clean").asProfile()->validity;
  const std::size_t kept = std::count(keptMask.begin(), keptMask.end(), std::uint8_t{1});
  CHECK(kept < all);

  Call strict;
  strict.inputs["profile"] = nativeCloud(resampled(td::kZ1000, td::kI1000));
  REQUIRE(strict.run("profile.clean", {{"minIntensity", Value::number(140.0)}}).ok);
  const auto& fewMask = strict.out("clean").asProfile()->validity;
  const std::size_t few = std::count(fewMask.begin(), fewMask.end(), std::uint8_t{1});
  CHECK(few < kept);
  MESSAGE("亮度门限 0.5 留 " << kept << " 点，门限 140 留 " << few << " 点，亮度一律 1.0 时留 "
                             << all);

  // 出去的云仍带亮度通道，且与点数等长 —— 下游 seal_dome 走的是同一片云。
  const PointCloud* out = defaulted.out("clean").asProfile()->cloud.get();
  REQUIRE(out->hasIntensity());
  CHECK(out->intensity.size() == out->pointCount());
}

TEST_CASE("现场样本：测量有效，A / B 与宿主 Python 的面差相差 0.3 mm 以内") {
  for (int i = 0; i < td::kFieldProfileCount; ++i) {
    const td::FieldProfile& fp = td::kFieldProfiles[i];
    const std::string tag(fp.tag);
    CAPTURE(tag);
    Chain chain;
    REQUIRE_MESSAGE(chain.run(cloudOf(fp.x, fp.z, fp.n)),
                    tag << " 停在 " << chain.failedOp << ": " << chain.status.message);
    const Measurement* m = chain.flush.out("flush").asMeasurement();
    REQUIRE(m != nullptr);
    CHECK(m->ok);
    CHECK(std::isfinite(m->value));
    const double delta = m->value - fp.flushPyMm;
    MESSAGE(tag << "（" << std::string(fp.ply) << "，Python 参数组 " << std::string(fp.paramSet)
                << "） dts = " << m->value << " mm，Python = " << fp.flushPyMm
                << " mm，Δ = " << delta << " mm");
    if (tag != "D")
      CHECK(std::fabs(delta) < 0.3);
  }
}

TEST_CASE("空轮廓在 profile_clean 就报 bad_input") {
  Call call;
  call.inputs["profile"] = nativeCloud(PointCloud{});
  const Status s = call.run("profile.clean");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "profile");
}

TEST_CASE("z 全是 NaN：滑动中值不补洞，profile_clean 报有效点太少") {
  PointCloud c;
  c.reserve(static_cast<std::size_t>(td::kPoints));
  for (int i = 0; i < td::kPoints; ++i) {
    c.push(td::kX0Mm + static_cast<float>(i) * td::kDxMm, 0.0f,
           std::numeric_limits<float>::quiet_NaN());
  }
  c.intensity.assign(static_cast<std::size_t>(td::kPoints), 1.0f);
  Call call;
  call.inputs["profile"] = nativeCloud(std::move(c));
  const Status s = call.run("profile.clean");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.message.find("有效点太少") != std::string::npos);
  CHECK(s.message.find("0") != std::string::npos);
}

TEST_CASE("亮度全 0：一个点都留不下，且错误话里带得出点数") {
  std::vector<unsigned char> dark(static_cast<std::size_t>(td::kPoints), 0);
  Call call;
  call.inputs["profile"] = nativeCloud(resampled(td::kZ1000, dark.data()));
  const Status s = call.run("profile.clean");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.message.find("有效点太少（0）") != std::string::npos);
  // 报错就不出货：clean 端口一个值都没写，下游拿到的是 Kind::None。
  CHECK(call.out("clean").asProfile() == nullptr);
}

TEST_CASE("一条直线没有胶条：停在 seal_dome") {
  // 完全平的一段：split_faces 切得出面，findDome 找不到凸起。
  PointCloud c;
  const int n = 800;
  c.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float x = -20.0f + static_cast<float>(i) * 0.05f;
    c.push(x, 0.0f, 0.2f * x);
  }
  c.intensity.assign(static_cast<std::size_t>(n), 1.0f);

  Chain chain;
  CHECK_FALSE(chain.run(std::move(c)));
  CHECK(chain.failedOp == "dts.seal_dome");
  CHECK(chain.status.code == "no_dome");
}

TEST_CASE("DTS preserves slots, channels, source identity and metric geometry spaces") {
  auto raw = resampled(td::kZ1000, td::kI1000);
  raw.normals.assign(raw.xyz.size(), .25f);
  raw.rgb.assign(raw.xyz.size(), 71);
  const auto originalIntensity = raw.intensity;
  Chain chain;
  REQUIRE_MESSAGE(chain.run(std::move(raw)), chain.failedOp << ": " << chain.status.message);
  const auto* clean = chain.clean.out("clean").asProfile();
  REQUIRE(clean);
  CHECK(clean->pointCount() == static_cast<std::size_t>(td::kPoints));
  CHECK(clean->cloud->intensity == originalIntensity);
  CHECK(clean->cloud->normals == std::vector<float>(clean->pointCount() * 3, .25f));
  CHECK(clean->cloud->rgb == std::vector<std::uint8_t>(clean->pointCount() * 3, 71));
  const auto& root = chain.root.out("root").asRecord()->data;
  CHECK(root["schemaVersion"] == 3);
  CHECK(root["workingUnit"] == "mm");
  const auto slot = root["slotIndex"].get<std::size_t>();
  CHECK(clean->validity.at(slot) == 1);
  CHECK(root["sourceIndex"] == clean->sourceIndices.at(slot));
  std::vector<std::size_t> active;
  for (std::size_t i = 0; i < clean->pointCount(); ++i)
    if (clean->validity[i])
      active.push_back(i);
  CHECK(active.at(root["activeIndex"].get<std::size_t>()) == slot);
  CHECK(root["legacySortedIndex"].is_number_integer());
  const auto* point = chain.root.out("point").asPoint2D();
  REQUIRE(point);
  CHECK(point->unit == Unit2D::Meter);
  CHECK(
      point->p[0] ==
      static_cast<float>(static_cast<double>(static_cast<float>(root["x"].get<double>())) * .001));
  CHECK(
      point->p[1] ==
      static_cast<float>(static_cast<double>(static_cast<float>(root["z"].get<double>())) * .001));
  std::string why;
  ProfileDescriptor descriptor;
  REQUIRE(profile_contract::geometrySpace(
      chain.root.out("point"), chain.root.out("pointContext").asRecord(), descriptor, why));
  REQUIRE(profile_contract::geometryInSpace(chain.metal.out("metal"),
                                            chain.metal.out("metalContext").asRecord(),
                                            clean->descriptor, why));
  CHECK(descriptor.frame == "dts-scan");
  const auto& info = chain.flush.out("footInfo").asRecord()->data;
  const auto& bundle = chain.bundle.out("bundle").asRecord()->data;
  CHECK(info["footMm"][0] == bundle["perpendicular"]["x"]);
  CHECK(info["footMm"][1] == bundle["perpendicular"]["z"]);
}

TEST_CASE("DTS rejects a non-XZ profile and missing fields without output") {
  Chain chain;
  REQUIRE(chain.run(resampled(td::kZ1000, nullptr)));
  const Data clean = chain.clean.out("clean");
  const auto checkRejected = [](Call& call, const char* id) {
    const auto status = call.run(id);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "bad_input");
    CHECK(call.outputs.empty());
  };
  auto changed = *clean.asProfile();
  changed.descriptor.plane = "XY";
  Call dome;
  dome.inputs = chain.dome.inputs;
  dome.inputs["clean"] = Data::profile(changed);
  checkRejected(dome, "dts.seal_dome");
  dome.inputs = chain.dome.inputs;
  auto faces = *chain.faces.out("faces").asRecord();
  faces.data["pieces"][0].erase("i0");
  dome.inputs["faces"] = Data::record(faces);
  checkRejected(dome, "dts.seal_dome");
  Call metal;
  metal.inputs = chain.metal.inputs;
  auto body = *chain.dome.out("dome").asRecord();
  body.data.erase("centerX");
  metal.inputs["dome"] = Data::record(body);
  checkRejected(metal, "dts.pick_metal");
  Call root;
  root.inputs = chain.root.inputs;
  auto pick = *chain.metal.out("pick").asRecord();
  pick.data.erase("metalLineMm");
  root.inputs["pick"] = Data::record(pick);
  checkRejected(root, "dts.seal_root");
}

TEST_CASE("DTS v2 flush retains legacy float direction arithmetic and all business signs") {
  Chain chain;
  REQUIRE(chain.run(resampled(td::kZ1000, nullptr)));
  const auto original = *chain.root.out("root").asRecord();
  for (const int direction : {1, -1})
    for (const auto* bulge : {"+z", "-z"})
      for (const auto* positive : {"deeper", "toward_sensor"}) {
        CAPTURE(direction);
        CAPTURE(std::string(bulge));
        CAPTURE(std::string(positive));
        Line2D legacy;
        legacy.point[0] = 1.234567f;
        legacy.point[1] = -2.345678f;
        legacy.dir[0] = .8f * direction;
        legacy.dir[1] = .6f * direction;
        auto root = original;
        root.data["x"] = 4.567891;
        root.data["z"] = 7.654321;
        root.data["metalLineMm"] = lyflow::dts::lineMillimeters(legacy);
        Call flush;
        flush.inputs["root"] = Data::record(root);
        REQUIRE(flush
                    .run("dts.flush", {{"sealBulge", Value::text(bulge)},
                                       {"positiveDirection", Value::text(positive)}})
                    .ok);
        const double dx = legacy.dir[0], dz = legacy.dir[1];
        const double nx = dx >= 0 ? -dz : dz, nz = dx >= 0 ? dx : -dx;
        const double distance =
            (4.567891 - legacy.point[0]) * nx + (7.654321 - legacy.point[1]) * nz;
        const double toward = std::string(bulge) == "+z" ? distance : -distance;
        const double expected = std::string(positive) == "deeper" ? -toward : toward;
        CHECK(flush.out("flush").asMeasurement()->value == expected);
        const auto& info = flush.out("footInfo").asRecord()->data;
        CHECK(info["footMm"][0].get<float>() == static_cast<float>(4.567891 - distance * nx));
        CHECK(info["footMm"][1].get<float>() == static_cast<float>(7.654321 - distance * nz));
      }
}

TEST_CASE("DTS native graphs preserve both datum choices and the host result contract") {
  const Data native = nativeCloud(resampled(td::kZ1000, nullptr));
  int defaultFace = -1;
  for (const std::string variant : {"default", "adjacent"}) {
    CAPTURE(variant);
    const auto path = std::filesystem::path(LYFLOW_EXAMPLES_DIR).parent_path() / "solutions" /
                      "dts" / "graphs" / (variant + "-profile.lyflow.json");
    std::ifstream input(path);
    REQUIRE(input.good());
    const auto graph = nlohmann::json::parse(input);
    test::Session run(graph, {}, {}, false, 1, false, {{"n_in", "profile", native}});
    const auto& log = run.wait();
    INFO(nlohmann::json(log.events).dump());
    REQUIRE(log.runStatus() == "ok");
    Data result;
    REQUIRE(exec::ResultStore::instance().get(run.runId(), "n_bundle", "bundle", result));
    const auto* bundle = result.asRecord();
    REQUIRE(bundle);
    CHECK(bundle->type == "DtsProfileBundle.v3");
    CHECK(bundle->data["ok"] == true);
    CHECK(bundle->data["workingUnit"] == "mm");
    CHECK(bundle->data.contains("perpendicular"));
    const auto currentFace = bundle->data.at("pick").at("faceIndex").get<int>();
    if (variant == "default")
      defaultFace = currentFace;
    else
      CHECK(currentFace != defaultFace);
    const double expected = variant == "default" ? 29.405751121893076 : 0.028393018568152684;
    CHECK(bundle->data["flushMm"].get<double>() == doctest::Approx(expected).epsilon(1e-6));
    REQUIRE(exec::ResultStore::instance().get(run.runId(), "n_flush", "foot", result));
    REQUIRE(result.asPoint2D());
    CHECK(result.asPoint2D()->unit == Unit2D::Meter);
    REQUIRE(exec::ResultStore::instance().get(run.runId(), "n_clean", "clean", result));
    const auto* clean = result.asProfile();
    REQUIRE(clean);
    CHECK(clean->pointCount() == static_cast<std::size_t>(td::kPoints));
    CHECK(clean->descriptor.unit == "mm");
    CHECK(clean->descriptor.plane == "XZ");
    CHECK(clean->descriptor.frame == "dts-scan");
    CHECK(clean->descriptor.sampleOrder == "acquisition");
  }
}

TEST_CASE("DTS v2 cancellation leaves native inputs and scene outputs unchanged") {
  class Context final : public ExecContext {
  public:
    explicit Context(int stop) : stop_(stop) {}
    bool cancelled() const override { return ++calls_ >= stop_; }
    void progress(float, std::string_view) override {}
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return base_; }
    int threadBudget() const override { return 1; }

  private:
    int stop_;
    mutable int calls_ = 0;
    std::filesystem::path base_;
  };
  Chain chain;
  REQUIRE(chain.run(resampled(td::kZ1000, nullptr)));
  const std::pair<const char*, Call*> calls[] = {{"dts.seal_dome", &chain.dome},
                                                 {"dts.pick_metal", &chain.metal},
                                                 {"dts.seal_root", &chain.root},
                                                 {"dts.flush", &chain.flush},
                                                 {"dts.profile_bundle", &chain.bundle}};
  for (const auto& entry : calls) {
    CAPTURE(std::string(entry.first));
    const auto* op = packRegistry().find(entry.first);
    REQUIRE(op);
    std::unordered_map<std::string, Data> values;
    Outputs outputs(values);
    Inputs inputs(entry.second->inputs);
    const std::filesystem::path base;
    ParamView params(entry.second->params, base);
    Context context(1);
    const auto status = op->compute(inputs, params, outputs, context);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "cancelled");
    CHECK(values.empty());
  }
  const auto* op = packRegistry().find("dts.seal_dome");
  std::unordered_map<std::string, Data> values;
  Outputs outputs(values);
  Inputs inputs(chain.dome.inputs);
  const std::filesystem::path base;
  ParamView params(chain.dome.params, base);
  Context context(25);
  const auto status = op->compute(inputs, params, outputs, context);
  CHECK_FALSE(status.ok);
  CHECK(status.code == "cancelled");
  CHECK(values.empty());
  CHECK(chain.clean.out("clean").asProfile()->pointCount() ==
        static_cast<std::size_t>(td::kPoints));
}

TEST_CASE("DTS v2 adjacent datum preserves the original A and B no-metal-face failures") {
  const auto path = std::filesystem::path(LYFLOW_EXAMPLES_DIR).parent_path() / "solutions" / "dts" /
                    "graphs" / "adjacent-profile.lyflow.json";
  std::ifstream input(path);
  REQUIRE(input.good());
  const auto graph = nlohmann::json::parse(input);
  for (const auto& field : td::kFieldProfiles) {
    const std::string tag(field.tag);
    if (tag != "A" && tag != "B")
      continue;
    CAPTURE(tag);
    test::Session run(graph, {}, {}, false, 1, false,
                      {{"n_in", "profile", nativeCloud(cloudOf(field.x, field.z, field.n))}});
    const auto& log = run.wait();
    REQUIRE(log.runStatus() == "error");
    CHECK(log.finalState("n_metal") == "error");
    CHECK(log.nodeEvent("n_metal", "error")["error"]["code"] == "no_metal_face");
    Data absent;
    CHECK_FALSE(exec::ResultStore::instance().get(run.runId(), "n_flush", "flush", absent));
  }
}

TEST_CASE("DTS retired operators report the obsolete node and next input contract") {
  const std::pair<const char*, const char*> cases[] = {{"dts.profile_in", "profile.input"},
                                                       {"dts.geometry_to_metric", "Point2D"},
                                                       {"dts.profile_clean", "profile.clean"},
                                                       {"dts.split_faces", "profile.split_faces"}};
  for (const auto& [retired, hint] : cases) {
    CAPTURE(std::string(retired));
    const auto graph = test::makeGraph({{"old", retired}}, {});
    const auto plan = nlohmann::json::parse(exec::planGraphJson(graph.dump(), {}, {}));
    INFO(plan.dump());
    CHECK(std::any_of(plan.begin(), plan.end(), [hint](const auto& item) {
      return item.value("nodeId", "") == "old" && item.value("code", "") == "migration_required" &&
             item.value("message", "").find(hint) != std::string::npos;
    }));
  }
}
