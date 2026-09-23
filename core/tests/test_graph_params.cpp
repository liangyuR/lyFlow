// 顶层图参数（m7-plan J7/J8）与算子的加载期校验钩子（J5）。
// 顶层参数的语义与子图提升参数相同：展开期把值写进节点参数，所以缓存键、plan、
// 生效参数视图不需要各自认识它 —— 这里钉住的就是「它们确实都看得见」。
#include <doctest/doctest.h>

#include "exec/executor.h"
#include "helpers.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

/// src(test.counted) → thin(test.thin) → tail(test.thin)，外加一支不相干的 side(test.counted)。
/// 顶层参数 count 绑 src.pointCount。
Json paramGraph(const Json& params = Json{{"count", {{"default", 40}, {"binds", {"src.pointCount"}}}}}) {
  Json doc = makeGraph({N{"src", "test.counted", Json::object()},
                        N{"thin", "test.thin", Json::object()},
                        N{"tail", "test.thin", Json::object()},
                        N{"side", "test.counted", Json{{"pointCount", 7}}}},
                       {E{"src.cloud", "thin.cloud"}, E{"thin.cloud", "tail.cloud"}});
  doc["params"] = params;
  return doc;
}

Json rowOf(const Json& view, const std::string& node, const std::string& param) {
  for (const Json& n : view["nodes"]) {
    if (n["node"] != node) continue;
    for (const Json& p : n["params"]) {
      if (p["param"] == param) return p;
    }
  }
  FAIL("没有 ", node, ".", param, "：", view.dump());
  return Json::object();
}

std::map<std::string, std::string> keysOf(const std::string& planJson) {
  std::map<std::string, std::string> keys;
  for (const Json& n : Json::parse(planJson)) {
    REQUIRE(n.contains("cacheKey"));
    keys[n["nodeId"].get<std::string>()] = n["cacheKey"].get<std::string>();
  }
  return keys;
}

bool hasDiag(const Json& diags, const std::string& code, const std::string& node = {}) {
  for (const Json& d : diags) {
    if (d.value("code", "") == code && (node.empty() || d.value("nodeId", "") == node)) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("顶层参数：绑定在展开期写进节点，生效值与 source=graph 由 core 说") {
  ensureTestOps();
  const Json view = Json::parse(exec::effectiveParamsJson(paramGraph().dump(), {}));
  const Json row = rowOf(view, "src", "pointCount");
  CHECK(row["value"] == 40);
  CHECK(row["source"] == "graph");
  CHECK(row["graphParam"] == "count");
  // 宿主传值取代 default
  const Json given =
      Json::parse(exec::effectiveParamsJson(paramGraph().dump(), {}, R"({"count": 55})"));
  CHECK(rowOf(given, "src", "pointCount")["value"] == 55);
  // 没被绑定的参数照旧
  CHECK(rowOf(given, "side", "pointCount")["source"] == "explicit");
}

TEST_CASE("顶层参数：换一个值只改被绑定节点及其下游的 cacheKey") {
  ensureTestOps();
  const std::string doc = paramGraph().dump();
  const auto a = keysOf(exec::planGraphJson(doc, {}, {}));
  const auto b = keysOf(exec::planGraphJson(doc, {}, {}, R"({"count": 41})"));
  CHECK(a.at("src") != b.at("src"));
  CHECK(a.at("thin") != b.at("thin"));
  CHECK(a.at("tail") != b.at("tail"));
  CHECK(a.at("side") == b.at("side"));
  // 传的值等于 default：键不变（绑定值与 default 是同一份东西）
  const auto same = keysOf(exec::planGraphJson(doc, {}, {}, R"({"count": 40})"));
  CHECK(same == a);
}

TEST_CASE("顶层参数：运行期取值真的进了 compute") {
  ensureTestOps();
  exec::RunOptions options;
  exec::ResultStore::instance().clear();
  RunLog log;
  log.runId = "graph-params-run";
  options.runId = log.runId;
  options.paramsJson = R"({"count": 123})";
  {
    exec::Run run(paramGraph().dump(), options, &detail::collect, &log);
    run.join();
  }
  REQUIRE(log.runStatus() == "ok");
  const Json done = log.nodeEvent("src", "done");
  REQUIRE(done.contains("stats"));
  CHECK(done["stats"]["outputs"][0]["elementCount"] == 123);
}

TEST_CASE("顶层参数：unknown_bind、param_conflict、未声明的名字") {
  ensureTestOps();
  SUBCASE("绑到不存在的节点") {
    const Json diags = Json::parse(exec::validateGraphJson(
        paramGraph(Json{{"count", {{"default", 1}, {"binds", {"nope.pointCount"}}}}}).dump(), {}));
    CHECK(hasDiag(diags, "unknown_bind"));
  }
  SUBCASE("绑到算子没有的参数") {
    const Json diags = Json::parse(exec::validateGraphJson(
        paramGraph(Json{{"count", {{"default", 1}, {"binds", {"src.nope"}}}}}).dump(), {}));
    CHECK(hasDiag(diags, "unknown_bind", "src"));
  }
  SUBCASE("被绑定的参数又在节点里显式写了") {
    Json doc = paramGraph();
    doc["nodes"][0]["params"]["pointCount"] = 9;
    const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
    CHECK(hasDiag(diags, "param_conflict", "src"));
  }
  SUBCASE("同一个目标被两个顶层参数绑定") {
    const Json diags = Json::parse(exec::validateGraphJson(
        paramGraph(Json{{"a", {{"default", 1}, {"binds", {"src.pointCount"}}}},
                        {"b", {{"default", 2}, {"binds", {"src.pointCount"}}}}})
            .dump(),
        {}));
    CHECK(hasDiag(diags, "param_conflict", "src"));
  }
  SUBCASE("宿主传了图没声明的名字") {
    const Json diags =
        Json::parse(exec::validateGraphJson(paramGraph().dump(), {}, R"({"nope": 1})"));
    CHECK(hasDiag(diags, "unknown_param"));
  }
  SUBCASE("binds 写法不对是 bad_input") {
    const Json diags = Json::parse(exec::validateGraphJson(
        paramGraph(Json{{"count", {{"default", 1}, {"binds", {"nodot"}}}}}).dump(), {}));
    CHECK(hasDiag(diags, "bad_input"));
  }
}

TEST_CASE("顶层参数绑到子图实例：里面的节点也报 source=graph") {
  ensureTestOps();
  Json doc;
  doc["schemaVersion"] = 1;
  doc["id"] = "01GRAPHPARAMSUB";
  doc["nodes"] = Json::array({Json{{"id", "src"}, {"op", "test.counted"}}, Json{{"id", "s"}, {"op", "sub:clean"}}});
  doc["edges"] = Json::array({Json{{"id", "e1"},
                                   {"from", {{"node", "src"}, {"port", "cloud"}}},
                                   {"to", {{"node", "s"}, {"port", "cloud"}}}}});
  doc["subgraphs"]["clean"] = Json{
      {"nodes", Json::array({Json{{"id", "v"}, {"op", "test.thin"}}})},
      {"edges", Json::array()},
      {"inputs", Json::array({Json{{"name", "cloud"},
                                   {"type", "PointCloud"},
                                   {"to", Json::array({Json{{"node", "v"}, {"port", "cloud"}}})}}})},
      {"outputs", Json::array({Json{{"name", "cloud"},
                                    {"type", "PointCloud"},
                                    {"from", {{"node", "v"}, {"port", "cloud"}}}}})},
      {"params", Json::array({Json{{"name", "leaf"},
                                   {"type", "vec3f"},
                                   {"default", Json::array({0.02, 0.02, 0.02})},
                                   {"binds", Json::array({Json{{"node", "v"}, {"param", "leaf"}}})}}})},
  };
  doc["params"] = Json{{"leafAll", {{"default", Json::array({0.05, 0.05, 0.05})}, {"binds", {"s.leaf"}}}}};
  const Json view = Json::parse(exec::effectiveParamsJson(doc.dump(), {}));
  const Json row = rowOf(view, "s/v", "leaf");
  CHECK(row["source"] == "graph");
  CHECK(row["graphParam"] == "leafAll");
  CHECK(row["value"][0] == doctest::Approx(0.05));

  // 绑到子图没声明的外参
  doc["params"]["leafAll"]["binds"] = Json::array({"s.nope"});
  CHECK(hasDiag(Json::parse(exec::validateGraphJson(doc.dump(), {})), "unknown_bind", "s"));
}

TEST_CASE("validate 钩子：error 进 Validate 诊断且节点不执行，warning 只提醒") {
  ensureTestOps();
  const Json bad = makeGraph({N{"src", "test.counted", Json{{"pointCount", 8}}},
                              N{"chk", "test.validated", Json{{"mode", "pinned"}}}},
                             {E{"src.cloud", "chk.cloud"}});
  const Json diags = Json::parse(exec::validateGraphJson(bad.dump(), {}));
  REQUIRE(diags.size() == 1);
  CHECK(diags[0]["nodeId"] == "chk");
  CHECK(diags[0]["severity"] == "error");
  CHECK(diags[0]["code"] == "bad_param");
  CHECK(diags[0]["phase"] == "validate");
  CHECK(diags[0]["paramPath"] == "mode");
  // plan 被阻断：返回的是诊断数组而不是计划
  const Json plan = Json::parse(exec::planGraphJson(bad.dump(), {}, {}));
  CHECK_FALSE(plan[0].contains("cacheKey"));
  // 跑一次：chk 从头到尾没进过 running
  const RunLog log = runGraph(bad);
  CHECK(log.nodeEvent("chk", "running").empty());
  CHECK(log.finalState("chk") == "error");

  // 接上 ref 就干净
  const Json good = makeGraph({N{"src", "test.counted", Json{{"pointCount", 8}}},
                               N{"chk", "test.validated", Json{{"mode", "pinned"}}}},
                              {E{"src.cloud", "chk.cloud"}, E{"src.cloud", "chk.ref"}});
  CHECK(Json::parse(exec::validateGraphJson(good.dump(), {})).empty());

  // warning：出现在 validate 里，但不挡执行
  const Json loud = makeGraph({N{"src", "test.counted", Json{{"pointCount", 8}}},
                               N{"chk", "test.validated", Json{{"loud", true}}}},
                              {E{"src.cloud", "chk.cloud"}});
  const Json warns = Json::parse(exec::validateGraphJson(loud.dump(), {}));
  REQUIRE(warns.size() == 1);
  CHECK(warns[0]["severity"] == "warning");
  CHECK(warns[0]["code"] == "loud");
  const RunLog ran = runGraph(loud);
  CHECK(ran.runStatus() == "ok");
  bool logged = false;
  for (const Json& e : ran.ofKind("log")) {
    if (e.value("level", "") == "warn" && e.value("nodeId", "") == "chk") logged = true;
  }
  CHECK(logged);
}

}  // namespace lyflow::test
