// transform 与 curve 两种参数的值格式（param-recipe P2.6，docs/operator-manifest.md「transform 与 curve 的值」），
// 以及参数面板的全类型示例算子 test.param_showcase 自己合不合规（P2.10）。
#include <doctest/doctest.h>

#include "exec/executor.h"
#include "helpers.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

Json showcaseGraph(const Json& params = Json::object()) {
  return makeGraph({N{"src", "gen.synthetic", Json{{"pointCount", 500}}},
                    N{"show", "test.param_showcase", params}},
                   {E{"src.cloud", "show.cloud"}});
}

Json validate(const Json& doc) { return Json::parse(exec::validateGraphJson(doc.dump(), {})); }

/// show.<param> 上的 bad_param。没有返回空串，有就返回消息。
std::string badParam(const Json& diags, const std::string& param) {
  for (const Json& d : diags) {
    if (d.value("code", "") == "bad_param" && d.value("nodeId", "") == "show" &&
        d.value("paramPath", "") == param) {
      return d.value("message", "");
    }
  }
  return {};
}

const Json* findOperator(const Json& manifest, const std::string& id) {
  for (const Json& op : manifest["operators"]) {
    if (op["id"] == id) return &op;
  }
  return nullptr;
}

const Json* findParamIn(const Json& op, const std::string& name) {
  for (const Json& p : op["params"]) {
    if (p["name"] == name) return &p;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("test.param_showcase：14 种类型各有一个，注册表自检不挑它的毛病") {
  ensureTestOps();
  const Registry& r = ensureRegistry();
  for (const std::string& problem : r.validate()) {
    CHECK_MESSAGE(problem.find("test.param_showcase") == std::string::npos, problem);
  }
  const OperatorDesc* op = r.find("test.param_showcase");
  REQUIRE(op != nullptr);
  std::set<ParamType> types;
  bool advanced = false, visibleWhen = false, enabledWhen = false, roi = false, grouped = false;
  for (const Param& p : op->params) {
    types.insert(p.type);
    advanced = advanced || p.advanced;
    visibleWhen = visibleWhen || p.visibleWhen.isSet();
    enabledWhen = enabledWhen || p.enabledWhen.isSet();
    roi = roi || p.semantic == "roi";
    grouped = grouped || !p.group.empty();
  }
  CHECK(types.size() == 14);
  CHECK(advanced);
  CHECK(visibleWhen);
  CHECK(enabledWhen);
  CHECK(roi);
  CHECK(grouped);
}

TEST_CASE("manifest 导出：curve 的默认值是对象，flags 的选项值是整数") {
  ensureTestOps();
  const Json manifest = Json::parse(ensureRegistry().toManifestJson());
  const Json* op = findOperator(manifest, "test.param_showcase");
  REQUIRE(op != nullptr);
  const Json* response = findParamIn(*op, "response");
  REQUIRE(response != nullptr);
  CHECK((*response)["default"].is_object());
  CHECK((*response)["default"]["interp"] == "smooth");
  CHECK((*response)["default"]["points"].size() == 3);
  const Json* features = findParamIn(*op, "features");
  REQUIRE(features != nullptr);
  for (const Json& o : (*features)["options"]) CHECK(o["value"].is_number_integer());
  CHECK((*features)["options"][2]["value"] == 4);
  const Json* pose = findParamIn(*op, "pose");
  REQUIRE(pose != nullptr);
  CHECK((*pose)["default"].size() == 16);
}

TEST_CASE("curve / transform 的值：合法的通过，形状不对的报 bad_param 并说清楚哪里不对") {
  ensureTestOps();
  CHECK(validate(showcaseGraph()).empty());
  const Json threePoints =
      Json{{"response", {{"points", {{0, 0.1}, {0.3, 0.9}, {1, 0.4}}}, {"interp", "linear"}}}};
  CHECK(validate(showcaseGraph(threePoints)).empty());
  // interp 可以不写（= linear）
  const Json noInterp = Json{{"response", {{"points", {{0, 0}, {1, 1}}}}}};
  CHECK(validate(showcaseGraph(noInterp)).empty());

  const auto bad = [](const Json& value) {
    return badParam(validate(showcaseGraph(Json{{"response", value}})), "response");
  };
  CHECK(bad(Json::array({0, 1})).find("对象") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}}}}).find("两个控制点") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {1.5, 1}}}}).find("[0, 1]") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {0.5, 1}, {0.5, 0.2}}}}).find("大于前一个点") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {1, 1.2}}}}).find("不能大于 1") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {1, 1}}}, {"interp", "cubic"}}).find("interp") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {1, 1}}}, {"tension", 1}}).find("tension") != std::string::npos);
  CHECK(bad(Json{{"points", {{0, 0}, {1, "a"}}}}).find("[x, y]") != std::string::npos);

  // transform 的值：16 个数的行主序矩阵，长度不对报 bad_param
  Json pose = Json::array();
  for (int i = 0; i < 16; ++i) pose.push_back(i % 5 == 0 ? 1.0 : 0.0);
  pose[3] = 0.25;  // 平移 x
  CHECK(validate(showcaseGraph(Json{{"pose", pose}})).empty());
  Json shortPose = pose;
  shortPose.erase(shortPose.begin());
  CHECK(badParam(validate(showcaseGraph(Json{{"pose", shortPose}})), "pose").find("16") !=
        std::string::npos);
}

TEST_CASE("curve 进 cacheKey 与键的书写顺序无关；两种值都原样进了 compute") {
  ensureTestOps();
  const Json a = showcaseGraph(Json{{"response", {{"points", {{0, 0}, {1, 0.5}}}, {"interp", "linear"}}}});
  const Json b = Json::parse(
      R"({"schemaVersion":1,"id":"01TESTTESTTESTTESTTESTTEST","nodes":[
          {"id":"src","op":"gen.synthetic","params":{"pointCount":500}},
          {"id":"show","op":"test.param_showcase","params":{"response":{"interp":"linear","points":[[0,0],[1,0.5]]}}}],
          "edges":[{"id":"e0","from":{"node":"src","port":"cloud"},"to":{"node":"show","port":"cloud"}}]})");
  const auto keyOf = [](const Json& doc) {
    for (const Json& n : Json::parse(exec::planGraphJson(doc.dump(), {}, {}))) {
      if (n.value("nodeId", "") == "show") return n.value("cacheKey", "");
    }
    return std::string();
  };
  CHECK_FALSE(keyOf(a).empty());
  CHECK(keyOf(a) == keyOf(b));

  Json pose = Json::array();
  for (int i = 0; i < 16; ++i) pose.push_back(i % 5 == 0 ? 1.0 : 0.0);
  pose[7] = -0.5;
  Json withPose = a;
  withPose["nodes"][1]["params"]["pose"] = pose;
  const RunLog log = runGraph(withPose);
  REQUIRE(log.runStatus() == "ok");
  const Json done = log.nodeEvent("show", "done");
  REQUIRE(done.contains("stats"));
  const Json* echo = nullptr;
  for (const Json& o : done["stats"]["outputs"]) {
    if (o["port"] == "echo") echo = &o;
  }
  REQUIRE(echo != nullptr);
  const Json data = (*echo)["value"]["data"];
  CHECK(data["params"]["pose"] == pose);
  CHECK(data["params"]["response"]["points"][1][1].get<double>() == doctest::Approx(0.5));
  // linear：x = 0.5 处正好一半
  CHECK(data["responseAt"][2].get<double>() == doctest::Approx(0.25));
}

TEST_CASE("evaluateCurve：线性插值、端点外取端点值、smooth 不冲过相邻点") {
  const Json linear = Json{{"points", {{0.2, 1.0}, {0.6, 3.0}}}};
  CHECK(evaluateCurve(linear, 0.4) == doctest::Approx(2.0));
  CHECK(evaluateCurve(linear, 0.0) == doctest::Approx(1.0));
  CHECK(evaluateCurve(linear, 1.0) == doctest::Approx(3.0));

  // 单调的控制点插出来的线也单调，且过每一个控制点
  const Json smooth = Json{{"interp", "smooth"}, {"points", {{0, 0}, {0.5, 0.35}, {0.6, 0.95}, {1, 1}}}};
  double prev = -1;
  for (int i = 0; i <= 100; ++i) {
    const double y = evaluateCurve(smooth, i / 100.0);
    CHECK(y >= prev - 1e-12);
    CHECK(y <= 1.0 + 1e-12);
    prev = y;
  }
  CHECK(evaluateCurve(smooth, 0.5) == doctest::Approx(0.35));
  CHECK(evaluateCurve(smooth, 0.6) == doctest::Approx(0.95));
  // 不合法的值取 0，不抛
  CHECK(evaluateCurve(Json::object(), 0.5) == 0.0);
}

}  // namespace lyflow::test
