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

namespace lyflow::packs::dts {
void registerPackOps(Registry& r);
}

namespace {

using namespace lyflow;
namespace td = lyflow::dts::testdata;

/// 不取消、不记进度的最小 ExecContext。
const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::dts::registerPackOps(reg);
    return reg;
  }();
  return r;
}

/// 直接调一个算子的 compute：参数先铺默认值，再用 overrides 覆盖。
/// 直接调 compute（test::OpCall），只用本包的注册表。
struct Call : test::OpCall {
  Call() : OpCall(packRegistry()) {}
};

/// default.lyflow.json 那条链，一步一步手搓。一步出错就停在那里，
/// `failedOp` 记住是哪一步 —— 失败路径的用例断言的就是它。
struct Chain {
  Call clean, faces, dome, metal, root, flush, bundle;
  std::unordered_map<std::string, Value> cleanOverrides;
  std::string failedOp;
  Status status;

  bool run(PointCloud raw) {
    clean.inputs["profile"] = Data::cloud(std::move(raw));
    if (!step(clean, "dts.profile_clean", cleanOverrides)) return false;

    faces.inputs["clean"] = clean.out("clean");
    if (!step(faces, "dts.split_faces")) return false;

    dome.inputs["clean"] = clean.out("clean");
    dome.inputs["faces"] = faces.out("faces");
    if (!step(dome, "dts.seal_dome")) return false;

    metal.inputs["clean"] = clean.out("clean");
    metal.inputs["faces"] = faces.out("faces");
    metal.inputs["dome"] = dome.out("dome");
    if (!step(metal, "dts.pick_metal")) return false;

    root.inputs["clean"] = clean.out("clean");
    root.inputs["faces"] = faces.out("faces");
    root.inputs["dome"] = dome.out("dome");
    root.inputs["pick"] = metal.out("pick");
    if (!step(root, "dts.seal_root")) return false;

    flush.inputs["root"] = root.out("root");
    flush.inputs["metal"] = metal.out("metal");
    if (!step(flush, "dts.flush")) return false;

    bundle.inputs["faces"] = faces.out("faces");
    bundle.inputs["dome"] = dome.out("dome");
    bundle.inputs["pick"] = metal.out("pick");
    bundle.inputs["root"] = root.out("root");
    bundle.inputs["metal"] = metal.out("metal");
    bundle.inputs["flush"] = flush.out("flush");
    bundle.inputs["foot"] = flush.out("foot");
    return step(bundle, "dts.profile_bundle");
  }

  double flushMm() { return flush.out("flush").asMeasurement()->value; }

 private:
  bool step(Call& c, const char* id,
            const std::unordered_map<std::string, Value>& overrides = {}) {
    status = c.run(id, overrides);
    if (!status.ok) failedOp = id;
    return status.ok;
  }
};

/// x/z 两个数组 → PointCloud。注入契约：y 恒 0，亮度有效点 1.0。
PointCloud cloudOf(const float* x, const float* z, int n) {
  PointCloud c;
  c.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) c.push(x[i], 0.0f, z[i]);
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

std::size_t cleanCount(Chain& c) { return c.clean.out("clean").asCloud()->pointCount(); }

}  // namespace

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
    REQUIRE_MESSAGE(chain.run(resampled(c.z, nullptr)), chain.failedOp << ": "
                                                                      << chain.status.message);

    // 清理只会丢点，不会造点；丢光了 profile_clean 自己就报错了，所以这里必 > 0。
    const std::size_t kept = cleanCount(chain);
    CHECK(kept > 0);
    CHECK(kept <= static_cast<std::size_t>(td::kPoints));

    const Record* faces = chain.faces.out("faces").asRecord();
    REQUIRE(faces != nullptr);
    CHECK(faces->type == "DtsFaces");
    CHECK(faces->data["faces"].size() >= 1);

    const Record* dome = chain.dome.out("dome").asRecord();
    REQUIRE(dome != nullptr);
    CHECK(dome->type == "DtsDome");
    // 半径是胶条的硬判据：seal_dome 成功就意味着它落在 [7, 14] 里。
    CHECK(dome->data["radiusMm"].get<double>() > 0.0);

    const Line2D* metal = chain.metal.out("metal").asLine2D();
    REQUIRE(metal != nullptr);
    CHECK(metal->hasSegment);

    const Record* root = chain.root.out("root").asRecord();
    REQUIRE(root != nullptr);
    CHECK(root->type == "DtsRoot");
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
    CHECK(bundle->type == "DtsProfileBundle");
    CHECK(bundle->data["ok"].get<bool>());
    CHECK(bundle->data["flushMm"].get<double>() == doctest::Approx(m->value));
  }
}

TEST_CASE("profile_clean 的 minIntensity 按亮度裁点") {
  // kI1000 是原始亮度（0..255），0 是 DP2240 的无效点。默认门限 0.5 只砍掉那 155 个；
  // 门限提到 140 就连低亮度的点一起砍 —— 这条是为了证明门限真的在起作用，
  // 不是「反正 z 都有限所以一个不丢」。
  Call plain;
  plain.inputs["profile"] = Data::cloud(resampled(td::kZ1000, nullptr));
  REQUIRE(plain.run("dts.profile_clean").ok);
  const std::size_t all = plain.out("clean").asCloud()->pointCount();
  CHECK(all == static_cast<std::size_t>(td::kPoints));

  Call defaulted;
  defaulted.inputs["profile"] = Data::cloud(resampled(td::kZ1000, td::kI1000));
  REQUIRE(defaulted.run("dts.profile_clean").ok);
  const std::size_t kept = defaulted.out("clean").asCloud()->pointCount();
  CHECK(kept < all);

  Call strict;
  strict.inputs["profile"] = Data::cloud(resampled(td::kZ1000, td::kI1000));
  REQUIRE(strict.run("dts.profile_clean", {{"minIntensity", Value::number(140.0)}}).ok);
  const std::size_t few = strict.out("clean").asCloud()->pointCount();
  CHECK(few < kept);
  MESSAGE("亮度门限 0.5 留 " << kept << " 点，门限 140 留 " << few << " 点，亮度一律 1.0 时留 "
                              << all);

  // 出去的云仍带亮度通道，且与点数等长 —— 下游 seal_dome 走的是同一片云。
  const PointCloud* out = defaulted.out("clean").asCloud();
  REQUIRE(out->hasIntensity());
  CHECK(out->intensity.size() == out->pointCount());
}

TEST_CASE("现场样本：整条链跑得通，并打印与宿主 Python 旧算法的差值") {
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
                << "） dts = " << m->value << " mm，Python = " << fp.flushPyMm << " mm，Δ = "
                << delta << " mm");
  }
}

TEST_CASE("现场样本 A / B 与 Python 旧算法差在 0.3 mm 以内") {
  // 2026-09-17 的对照（packs/dts/README.md「与宿主 Python 旧算法的对照」）：
  // A 第 0 条 Δ = +0.212 mm、B 第 0 条 Δ = −0.110 mm。挑面挑的是同一族面，
  // 差值来自底部点的取法不同（Python 的 foot_arc_apex vs 这里的「凹-凸-凹」）。
  // D 不在这里 —— V 形脚那一条差 59 mm，原因见 README 的「已知差距」。
  const double kTolMm = 0.3;
  for (int i = 0; i < td::kFieldProfileCount; ++i) {
    const td::FieldProfile& fp = td::kFieldProfiles[i];
    const std::string tag(fp.tag);
    if (tag == "D") continue;
    CAPTURE(tag);
    Chain chain;
    REQUIRE(chain.run(cloudOf(fp.x, fp.z, fp.n)));
    CHECK(std::fabs(chain.flushMm() - fp.flushPyMm) < kTolMm);
  }
}

TEST_CASE("空轮廓在 profile_clean 就报 bad_input") {
  Call call;
  call.inputs["profile"] = Data::cloud(PointCloud{});
  const Status s = call.run("dts.profile_clean");
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
  call.inputs["profile"] = Data::cloud(std::move(c));
  const Status s = call.run("dts.profile_clean");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.message.find("有效点太少") != std::string::npos);
  CHECK(s.message.find("0") != std::string::npos);
}

TEST_CASE("亮度全 0：一个点都留不下，且错误话里带得出点数") {
  std::vector<unsigned char> dark(static_cast<std::size_t>(td::kPoints), 0);
  Call call;
  call.inputs["profile"] = Data::cloud(resampled(td::kZ1000, dark.data()));
  const Status s = call.run("dts.profile_clean");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.message.find("有效点太少（0）") != std::string::npos);
  // 报错就不出货：clean 端口一个值都没写，下游拿到的是 Kind::None。
  CHECK(call.out("clean").asCloud() == nullptr);
}

TEST_CASE("一条直线没有胶条：停在 seal_dome，下游不崩") {
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

  // seal_dome 失败时执行器不会调 dts.flush（它的两个端口都是必填且不 acceptsError，
  // 见 ADR-0016）。真调进去也只能是「类型不对」，不能是崩 —— 这条守的是那个不变量。
  Call orphan;
  const Status s = orphan.run("dts.flush");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "type_mismatch");
}
