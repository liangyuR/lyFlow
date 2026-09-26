// 端口契约（ADR-0024 / m6-plan §3）：四种键各一例、声明本身的自检、
// 「不声明零开销」（不走检查路径）、以及违反之后 summary 里那条 contractViolations。
#include <doctest/doctest.h>

#include <limits>

#include "exec/executor.h"
#include "helpers.h"
#include "lyflow/contract.h"
#include "lyflow/registry.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

/// count 端口是必填的，每个用例都要接一片**合规**的云，
/// 这样断言到的违反就一定来自被测的那一个端口。
N okCount() { return N{"n_count", "test.counted", Json{{"pointCount", 6}}}; }

Json summaryOf(RunLog& log) {
  const auto finished = log.ofKind("run_finished");
  REQUIRE(finished.size() == 1);
  REQUIRE(finished.back().contains("summary"));
  return finished.back()["summary"];
}

/// 一次运行 + 那一条 node_state 的错误 + summary 的 contractViolations。
struct Outcome {
  Json nodeError;
  Json violations;
};

Outcome runAndInspect(const Json& doc, const char* nodeId) {
  Session s(doc);
  RunLog& log = s.wait();
  Outcome out;
  const Json state = log.nodeEvent(nodeId, "error");
  if (state.contains("error")) out.nodeError = state["error"];
  out.violations = summaryOf(log)["contractViolations"];
  return out;
}

}  // namespace

TEST_CASE("契约：elementCount.eq —— 点数不对，第一帧就报 contract_violation") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_src", "test.counted", Json{{"pointCount", 5}}},
                              N{"n_c", "test.contracted", Json::object()}},
                             {E{"n_src.cloud", "n_c.count"}});
  const Outcome out = runAndInspect(doc, "n_c");
  REQUIRE(out.nodeError.is_object());
  CHECK(out.nodeError["code"] == "contract_violation");
  // 期望与实际都要在 message 里 —— 只说「契约不满足」的话还得自己去翻 manifest
  const auto message = out.nodeError["message"].get<std::string>();
  CHECK(message.find("6") != std::string::npos);
  CHECK(message.find("5") != std::string::npos);
  CHECK(out.nodeError["portName"] == "count");

  REQUIRE(out.violations.size() == 1);
  CHECK(out.violations[0]["node"] == "n_c");
  CHECK(out.violations[0]["port"] == "count");
  CHECK(out.violations[0]["expected"]["elementCount"]["eq"] == 6);
  CHECK(out.violations[0]["actual"]["elementCount"] == 5);
}

TEST_CASE("契约：finite —— 一个 NaN 坐标就够") {
  ensureTestOps();
  const Json doc =
      makeGraph({okCount(), N{"n_nan", "test.nonfinite_cloud", Json{{"pointCount", 6}, {"badAt", 2}}},
                 N{"n_c", "test.contracted", Json::object()}},
                {E{"n_count.cloud", "n_c.count"}, E{"n_nan.cloud", "n_c.finite"}});
  const Outcome out = runAndInspect(doc, "n_c");
  REQUIRE(out.nodeError.is_object());
  CHECK(out.nodeError["code"] == "contract_violation");
  CHECK(out.nodeError["portName"] == "finite");
  REQUIRE(out.violations.size() == 1);
  CHECK(out.violations[0]["port"] == "finite");
  CHECK(out.violations[0]["expected"]["finite"] == true);
  CHECK(out.violations[0]["actual"]["nonFiniteCoords"] == 1);

  // 同一条链、同一个端口，只是不带 NaN：一条违反都没有
  const Json good = makeGraph(
      {okCount(), N{"n_nan", "test.nonfinite_cloud", Json{{"pointCount", 6}, {"badAt", -1}}},
       N{"n_c", "test.contracted", Json::object()}},
      {E{"n_count.cloud", "n_c.count"}, E{"n_nan.cloud", "n_c.finite"}});
  Session s(good);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");
  CHECK(summaryOf(log)["contractViolations"].empty());
}

TEST_CASE("契约：shape —— -1 的那一维随便，定死的那两维必须对上") {
  ensureTestOps();
  // 声明是 [2, -1, 4]。[2, 9, 4] 过，[2, 3, 5] 不过。
  auto graphWith = [](int d1, int d2) {
    return makeGraph({okCount(),
                      N{"n_t", "test.make_tensor",
                        Json{{"dim0", 2}, {"dim1", d1}, {"dim2", d2}}},
                      N{"n_c", "test.contracted", Json::object()}},
                     {E{"n_count.cloud", "n_c.count"}, E{"n_t.tensor", "n_c.tensor"}});
  };
  {
    Session s(graphWith(9, 4));
    RunLog& log = s.wait();
    CHECK(log.runStatus() == "ok");
    CHECK(summaryOf(log)["contractViolations"].empty());
  }
  const Outcome out = runAndInspect(graphWith(3, 5), "n_c");
  REQUIRE(out.nodeError.is_object());
  CHECK(out.nodeError["code"] == "contract_violation");
  CHECK(out.nodeError["portName"] == "tensor");
  REQUIRE(out.violations.size() == 1);
  CHECK(out.violations[0]["expected"]["shape"] == Json::array({2, -1, 4}));
  CHECK(out.violations[0]["actual"]["shape"] == Json::array({2, 3, 5}));
}

TEST_CASE("契约：recordType —— Record 的 type 字串对不上") {
  ensureTestOps();
  const Json doc = makeGraph({okCount(), N{"n_r", "test.make_record", Json{{"type", "Other"}}},
                              N{"n_c", "test.contracted", Json::object()}},
                             {E{"n_count.cloud", "n_c.count"}, E{"n_r.rec", "n_c.rec"}});
  const Outcome out = runAndInspect(doc, "n_c");
  REQUIRE(out.nodeError.is_object());
  CHECK(out.nodeError["code"] == "contract_violation");
  CHECK(out.nodeError["portName"] == "rec");
  REQUIRE(out.violations.size() == 1);
  CHECK(out.violations[0]["expected"]["recordType"] == "Wanted");
  CHECK(out.violations[0]["actual"]["recordType"] == "Other");
}

TEST_CASE("契约：不声明就零开销 —— 同一批坏数据在无契约端口上一路畅通") {
  ensureTestOps();
  // test.uncontracted 与 test.contracted 是同一组端口，差别只有 contract。
  // 四份数据全是上面几个用例里被拒掉的那些，这里必须一条违反都没有。
  const Json doc = makeGraph(
      {N{"n_count", "test.counted", Json{{"pointCount", 5}}},
       N{"n_nan", "test.nonfinite_cloud", Json{{"pointCount", 6}, {"badAt", 0}}},
       N{"n_t", "test.make_tensor", Json{{"dim0", 7}, {"dim1", 1}, {"dim2", 1}}},
       N{"n_r", "test.make_record", Json{{"type", "Other"}}},
       N{"n_u", "test.uncontracted", Json::object()}},
      {E{"n_count.cloud", "n_u.count"}, E{"n_nan.cloud", "n_u.finite"},
       E{"n_t.tensor", "n_u.tensor"}, E{"n_r.rec", "n_u.rec"}});
  Session s(doc);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");
  const Json summary = summaryOf(log);
  CHECK(summary["status"] == "ok");
  CHECK(summary["contractViolations"].empty());

  // 直接问那个函数本身：空契约在碰 value 之前就返回 true。
  nlohmann::json expected;
  nlohmann::json actual;
  std::string message;
  PointCloud bad;
  bad.push(std::numeric_limits<float>::quiet_NaN(), 0, 0);
  CHECK(checkPortContract(nlohmann::json::object(), Data::cloud(std::move(bad)), expected, actual,
                          message));
  CHECK(message.empty());
}

TEST_CASE("契约：静音节点的透传不检查（bypass 只搬运，不声称自己算得对）") {
  ensureTestOps();
  // bypass：这个节点只是搬运，不声称自己算得对（ADR-0024）。
  Json doc = makeGraph({N{"n_src", "test.counted", Json{{"pointCount", 5}}},
                        N{"n_c", "test.contracted", Json::object()}},
                       {E{"n_src.cloud", "n_c.count"}});
  doc["nodes"][1]["bypass"] = true;
  {
    Session s(doc);
    RunLog& log = s.wait();
    CHECK(summaryOf(log)["contractViolations"].empty());
  }
}

TEST_CASE("契约自检：四种键之外的任何东西都过不了 manifest --check") {
  const std::string where = "op 'x' input port 'p'";
  CHECK(validatePortContract(nlohmann::json::object(), where).empty());
  CHECK(validatePortContract(nlohmann::json{{"elementCount", {{"eq", 1280}}}}, where).empty());
  CHECK(validatePortContract(nlohmann::json{{"finite", true}}, where).empty());
  CHECK(validatePortContract(nlohmann::json{{"shape", {2, -1, 1280}}}, where).empty());
  CHECK(validatePortContract(nlohmann::json{{"recordType", "GapLabels"}}, where).empty());

  // 第五种键：这一行就是「不做表达式语言」那条决定的执行处
  CHECK(validatePortContract(nlohmann::json{{"expr", "n > 3"}}, where).size() == 1);
  CHECK(validatePortContract(nlohmann::json{{"elementCount", {{"gt", 3}}}}, where).size() == 1);
  // finite:false 读起来像「这里允许 NaN」，而那是**没有契约**
  CHECK(validatePortContract(nlohmann::json{{"finite", false}}, where).size() == 1);
  CHECK(validatePortContract(nlohmann::json{{"elementCount", {{"min", 9}, {"max", 3}}}}, where)
            .size() == 1);
  CHECK(validatePortContract(nlohmann::json{{"elementCount", {{"eq", 6}, {"min", 3}}}}, where)
            .size() == 1);
  CHECK(validatePortContract(nlohmann::json{{"shape", Json::array({2, -3})}}, where).size() == 1);
  CHECK(validatePortContract(nlohmann::json{{"recordType", ""}}, where).size() == 1);

  // 注册表自检本身是干净的：包里声明的每一条契约都过得了这一关
  CHECK(ensureRegistry().validate().empty());
}

}  // namespace lyflow::test
