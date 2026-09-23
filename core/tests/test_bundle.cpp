// Bundle（m8-plan L1–L3）：一根线带一组有名字的字段。
// 字段表在 manifest 里声明、执行器按声明查；结果仓、C ABI、事件、summary、图输出按
// `<port>.<field>` 寻址；类型检查只认 kind 相等。
#include <doctest/doctest.h>

#include <string>

#include "exec/executor.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/c_api.h"
#include "lyflow/registry.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

Json summaryOf(RunLog& log) {
  const auto finished = log.ofKind("run_finished");
  REQUIRE(finished.size() == 1);
  REQUIRE(finished.back().contains("summary"));
  return finished.back()["summary"];
}

Json withOutputs(Json doc, const Json& outputs) {
  doc["outputs"] = outputs;
  return doc;
}

Json pairGraph(const char* mode) {
  return makeGraph({N{"n_pair", "test.make_pair", Json{{"mode", mode}}},
                    N{"n_take", "test.take_pair", Json::object()}},
                   {E{"n_pair.pair", "n_take.pair"}});
}

}  // namespace

TEST_CASE("Bundle：manifest 带 bundles 段，端口类型写作 Bundle<kind>") {
  ensureTestOps();
  const Json m = Json::parse(ensureRegistry().toManifestJson());
  REQUIRE(m.contains("bundles"));
  bool found = false;
  for (const Json& b : m["bundles"]) {
    if (b["kind"] != "test.Pair") continue;
    found = true;
    REQUIRE(b["fields"].size() == 2);
    CHECK(b["fields"][0]["name"] == "cloud");
    CHECK(b["fields"][0]["type"] == "PointCloud");
    CHECK(b["fields"][1]["name"] == "box");
  }
  CHECK(found);
  CHECK(ensureRegistry().knowsType("Bundle<test.Pair>"));
  CHECK_FALSE(ensureRegistry().knowsType("Bundle<test.Nope>"));
  CHECK(ensureRegistry().validate().empty());
}

TEST_CASE("Bundle：声明写错了 Registry::validate 就拒") {
  Registry r;
  for (const auto& t : ensureRegistry().types()) r.addType(t);
  r.addBundle(BundleDesc{"x.Bad", "", "", "",
                         {BundleField{"a", "Any", ""}, BundleField{"b", "Bundle<x.Bad>", ""},
                          BundleField{"c.d", "Box2D", ""}, BundleField{"e", "Nope", ""}}});
  OperatorDesc op;
  op.id = "x.op";
  op.version = "1.0.0";
  op.label = "X";
  op.category = "X";
  op.compute = &ops::anyPassCompute;
  op.inputs = {Port{"in", "Bundle<x.Undeclared>", "", "", true},
               Port{"bare", "Bundle", "", "", true}};
  op.outputs = {Port{"out", "Bundle<x.Bad>", "", "", true}};
  r.addOperator(op);
  std::string all;
  for (const auto& p : r.validate()) all += p + "\n";
  CHECK(all.find("field 'a' 的类型不能是 Any") != std::string::npos);
  CHECK(all.find("field 'b' 的类型不能是 Bundle<x.Bad>") != std::string::npos);
  CHECK(all.find("field 'c.d'") != std::string::npos);
  CHECK(all.find("uses unknown type 'Nope'") != std::string::npos);
  CHECK(all.find("uses unknown type 'Bundle<x.Undeclared>'") != std::string::npos);
  CHECK(all.find("裸的 Bundle") != std::string::npos);
  // 已声明的 kind 在端口上是合法的
  CHECK(all.find("port 'out'") == std::string::npos);
}

TEST_CASE("Bundle：字段齐全、类型对得上才放行，否则 contract_violation") {
  ensureTestOps();
  {
    RunLog log = runGraph(pairGraph("ok"));
    CHECK(log.runStatus() == "ok");
    CHECK(summaryOf(log)["contractViolations"].empty());
  }
  for (const char* mode : {"missing", "wrongType", "wrongKind", "extra"}) {
    CAPTURE(mode);
    RunLog log = runGraph(pairGraph(mode));
    const Json err = log.nodeEvent("n_pair", "error");
    REQUIRE(err.contains("error"));
    CHECK(err["error"]["code"] == "contract_violation");
    CHECK(err["error"]["portName"] == "pair");
    const Json v = summaryOf(log)["contractViolations"];
    REQUIRE(v.size() == 1);
    CHECK(v[0]["node"] == "n_pair");
    CHECK(v[0]["port"] == "pair");
    CHECK(v[0]["expected"]["bundle"] == "test.Pair");
    CHECK(v[0]["expected"]["fields"]["box"] == "Box2D");
    // 下游因为上游失败而不跑
    CHECK(log.finalState("n_take") == "cancelled");
  }
  {
    RunLog log = runGraph(pairGraph("missing"));
    const std::string message = log.nodeEvent("n_pair", "error")["error"]["message"];
    CHECK(message.find("缺字段 'box'") != std::string::npos);
  }
  {
    RunLog log = runGraph(pairGraph("wrongType"));
    const std::string message = log.nodeEvent("n_pair", "error")["error"]["message"];
    CHECK(message.find("应当是 Box2D，实际是 PointCloud") != std::string::npos);
  }
}

TEST_CASE("Bundle：Bundle<test.Other> 接 Bundle<test.Pair> 报 type_mismatch，接 Any 照常") {
  ensureTestOps();
  const Json bad = makeGraph({N{"n_other", "test.make_other", Json::object()},
                              N{"n_take", "test.take_pair", Json::object()}},
                             {E{"n_other.other", "n_take.pair"}});
  const Json diags = Json::parse(exec::validateGraphJson(bad.dump(), {}));
  bool mismatch = false;
  for (const Json& d : diags) {
    if (d.value("code", "") == "type_mismatch" && d.value("nodeId", "") == "n_take") {
      mismatch = true;
      CHECK(d["message"].get<std::string>().find("Bundle<test.Other> → Bundle<test.Pair>") !=
            std::string::npos);
    }
  }
  CHECK(mismatch);

  // Bundle 与普通类型之间也不隐式转换
  const Json bad2 = makeGraph({N{"n_pair", "test.make_pair", Json::object()},
                               N{"n_thin", "test.thin", Json::object()}},
                              {E{"n_pair.pair", "n_thin.cloud"}});
  CHECK(exec::validateGraphJson(bad2.dump(), {}).find("type_mismatch") != std::string::npos);

  // flow.fallback 的 Any 端口推导成 Bundle<test.Pair>，下游照常收到
  const Json ok = makeGraph({N{"n_a", "test.make_pair", Json{{"pointCount", 4}}},
                             N{"n_b", "test.make_pair", Json{{"pointCount", 9}}},
                             N{"n_fb", "flow.fallback", Json::object()},
                             N{"n_take", "test.take_pair", Json::object()}},
                            {E{"n_a.pair", "n_fb.a"}, E{"n_b.pair", "n_fb.b"},
                             E{"n_fb.out", "n_take.pair"}});
  Session s(ok);
  RunLog& log = s.wait();
  CHECK(log.runStatus() == "ok");
  Data got;
  REQUIRE(exec::ResultStore::instance().get(s.runId(), "n_take", "cloud", got));
  CHECK(got.asCloud()->pointCount() == 4);
}

TEST_CASE("Bundle：结果仓、C ABI、事件按 <port>.<field> 寻址") {
  ensureTestOps();
  Session s(makeGraph({N{"n_pair", "test.make_pair", Json{{"pointCount", 5}}}}, {}));
  RunLog& log = s.wait();
  REQUIRE(log.runStatus() == "ok");
  const std::string run = s.runId();

  // lyflow_output_cloud 不改签名，直接认 scan.merged 这种写法
  lyflow_cloud_view view{};
  REQUIRE(lyflow_output_cloud(run.c_str(), "n_pair", "pair.cloud", 0, &view) == 0);
  CHECK(view.total_points == 5u);
  lyflow_cloud_view_free(&view);
  CHECK(lyflow_output_cloud(run.c_str(), "n_pair", "pair.nope", 0, &view) == 1);
  // 字段不是点云时照旧返回 1
  CHECK(lyflow_output_cloud(run.c_str(), "n_pair", "pair.box", 0, &view) == 1);

  // lyflow_output_info 对 Bundle 端口列出每个字段
  char* raw = lyflow_output_info(run.c_str(), "n_pair");
  const Json info = Json::parse(raw);
  lyflow_string_free(raw);
  REQUIRE(info.size() == 3);
  CHECK(info[0]["port"] == "pair");
  CHECK(info[0]["type"] == "Bundle<test.Pair>");
  CHECK(info[0]["elementCount"] == 2);
  CHECK(info[0]["value"]["bundleKind"] == "test.Pair");
  CHECK(info[0]["value"]["fields"][1]["value"]["kind"] == "Box2D");
  CHECK(info[1]["port"] == "pair.cloud");
  CHECK(info[1]["type"] == "PointCloud");
  CHECK(info[1]["elementCount"] == 5);
  CHECK(info[2]["port"] == "pair.box");
  CHECK(info[2]["value"]["max"][1].get<double>() == doctest::Approx(2.0));

  // 事件里的 stats.outputs 也带字段
  const Json done = log.nodeEvent("n_pair", "done");
  const Json outs = done["stats"]["outputs"];
  REQUIRE(outs.size() == 3);
  CHECK(outs[1]["port"] == "pair.cloud");
  CHECK(outs[2]["port"] == "pair.box");

  // 缓存命中时事件照样带字段
  RunLog again = runGraphCached(makeGraph({N{"n_pair", "test.make_pair", Json{{"pointCount", 5}}}}, {}));
  const Json skipped = again.nodeEvent("n_pair", "skipped");
  REQUIRE(skipped.contains("stats"));
  CHECK(skipped["stats"]["outputs"].size() == 3);
}

TEST_CASE("Bundle：图输出可以指向字段，summary 与 lyflow_run_outputs 照常给值") {
  ensureTestOps();
  const Json doc =
      withOutputs(makeGraph({N{"n_pair", "test.make_pair", Json{{"pointCount", 6}}}}, {}),
                  Json{{"box", Json{{"node", "n_pair"}, {"port", "pair.box"}}},
                       {"cloud", Json{{"node", "n_pair"}, {"port", "pair.cloud"}}},
                       {"whole", Json{{"node", "n_pair"}, {"port", "pair"}}}});
  CHECK(Json::parse(exec::validateGraphJson(doc.dump(), {})).empty());
  Session s(doc);
  RunLog& log = s.wait();
  REQUIRE(log.runStatus() == "ok");

  const Json outputs = Json::parse(exec::runOutputsJson(s.runId()));
  CHECK(outputs["box"]["type"] == "Box2D");
  CHECK(outputs["box"]["value"]["max"][0].get<double>() == doctest::Approx(1.0));
  CHECK(outputs["cloud"]["type"] == "PointCloud");
  CHECK(outputs["cloud"]["elementCount"] == 6);
  CHECK(outputs["whole"]["type"] == "Bundle<test.Pair>");

  const Json summary = summaryOf(log);
  CHECK(summary["status"] == "ok");
  CHECK(summary["outputs"]["box"]["state"] == "value");
  CHECK(summary["outputs"]["box"]["port"] == "pair.box");
  CHECK(summary["outputs"]["cloud"]["elementCount"] == 6);

  // 字段不存在、或端口不是 Bundle，都是校验期的 unknown_port
  for (const char* port : {"pair.nope", "nope.box"}) {
    CAPTURE(port);
    const Json bad = withOutputs(makeGraph({N{"n_pair", "test.make_pair", Json::object()}}, {}),
                                 Json{{"x", Json{{"node", "n_pair"}, {"port", port}}}});
    CHECK(exec::validateGraphJson(bad.dump(), {}).find("unknown_port") != std::string::npos);
  }
  const Json notBundle = withOutputs(makeGraph({N{"g", "gen.synthetic", Json::object()}}, {}),
                                     Json{{"x", Json{{"node", "g"}, {"port", "cloud.x"}}}});
  CHECK(exec::validateGraphJson(notBundle.dump(), {}).find("不是 Bundle") != std::string::npos);
}

}  // namespace lyflow::test
