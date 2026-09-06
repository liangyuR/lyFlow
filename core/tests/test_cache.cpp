// M3 核心轨：缓存复用与 LRU、并行调度、bypass 透传、Any 推导、迁移链。
// 算子一律用 gen.synthetic / util.reroute / test.*（S6）。
#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <set>
#include <thread>

#include "exec/plan.h"
#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "test_ops.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

using Ms = std::chrono::milliseconds;

const Json kSmall = Json{{"pointCount", 4000}};

Json chainGraph() {
  return makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "test.thin", Json{{"leaf", {0.02, 0.02, 0.02}}}},
          {"p", "util.reroute"},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.in"}});
}

/// 1 源 → 4 支 → 1 汇。每支睡 sleepMs，tag 各不相同。
Json diamondGraph(int sleepMs) {
  std::vector<N> nodes{{"g", "gen.synthetic", kSmall}};
  std::vector<E> edges;
  for (int i = 0; i < 4; ++i) {
    const std::string id = "s" + std::to_string(i);
    nodes.push_back(N{id, "test.sleep", Json{{"ms", sleepMs}, {"tag", id}}});
    edges.push_back(E{"g.cloud", id + ".cloud"});
  }
  // 汇点靠 test.merge2 两两合并成一棵小树，因为输入端口是单连接的
  nodes.push_back(N{"m0", "test.merge2"});
  nodes.push_back(N{"m1", "test.merge2"});
  nodes.push_back(N{"m", "test.merge2"});
  edges.push_back(E{"s0.cloud", "m0.a"});
  edges.push_back(E{"s1.cloud", "m0.b"});
  edges.push_back(E{"s2.cloud", "m1.a"});
  edges.push_back(E{"s3.cloud", "m1.b"});
  edges.push_back(E{"m0.cloud", "m.a"});
  edges.push_back(E{"m1.cloud", "m.b"});
  return makeGraph(nodes, edges);
}

std::set<std::string> statesOf(const RunLog& log, const std::string& state) {
  std::set<std::string> out;
  for (const Json& e : log.events) {
    if (e.value("kind", "") == "node_state" && e.value("state", "") == state) {
      out.insert(e.value("nodeId", ""));
    }
  }
  return out;
}

double runDurationMs(const RunLog& log) {
  const auto finished = log.ofKind("run_finished");
  return finished.empty() ? -1.0 : finished.back().value("durationMs", -1.0);
}

}  // namespace

// ---------------------------------------------------------------- 1.1 缓存

TEST_CASE("原图重跑：全部 skipped，总耗时 < 50 ms") {
  ensureTestOps();
  const Json doc = chainGraph();
  const RunLog first = runGraph(doc);
  REQUIRE(first.runStatus() == "ok");
  CHECK(statesOf(first, "done").size() == 3);

  const RunLog second = runGraphCached(doc);
  CHECK(second.runStatus() == "ok");
  CHECK(second.seqIsDense());
  CHECK(statesOf(second, "skipped") == std::set<std::string>{"g", "v", "p"});
  CHECK(statesOf(second, "done").empty());
  CHECK(runDurationMs(second) < 50.0);

  // skipped 的事件要说清是「命中缓存」而不是「被静音」
  const Json v = second.nodeEvent("v", "skipped");
  CHECK(v["stats"]["cached"] == true);
  CHECK(v["stats"].contains("bypassed") == false);
  CHECK(v["stats"]["elementCount"].get<int>() > 0);
}

TEST_CASE("noReuse：本次全部重算，但别的 run 的结果还在仓里") {
  ensureTestOps();
  const Json doc = chainGraph();
  Session keeper(doc);
  REQUIRE(keeper.wait().runStatus() == "ok");

  const RunLog fresh = runGraphNoReuse(doc);
  CHECK(fresh.runStatus() == "ok");
  CHECK(statesOf(fresh, "done") == std::set<std::string>{"g", "v", "p"});
  CHECK(statesOf(fresh, "skipped").empty());

  // 关键：--no-cache 不再是进程级 clear，keeper 那次运行的索引必须完好
  Data out;
  CHECK(exec::ResultStore::instance().get(keeper.runId(), "v", "cloud", out));
}

TEST_CASE("改中间节点参数：只重算它和它的下游") {
  ensureTestOps();
  const RunLog first = runGraph(chainGraph());
  REQUIRE(first.runStatus() == "ok");

  const Json changed = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"v", "test.thin", Json{{"leaf", {0.05, 0.05, 0.05}}}},
          {"p", "util.reroute"},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.in"}});
  const RunLog second = runGraphCached(changed);

  CHECK(second.runStatus() == "ok");
  CHECK(second.finalState("g") == "skipped");  // 上游没变
  CHECK(second.finalState("v") == "done");
  CHECK(second.finalState("p") == "done");     // 下游跟着失效
}

TEST_CASE("lyflow_plan 的预测集合与实际 skipped 集合完全一致") {
  ensureTestOps();
  const Json doc = chainGraph();
  exec::ResultStore::instance().clear();

  const Json cold = Json::parse(exec::planGraphJson(doc.dump(), {}, {}));
  REQUIRE(cold.is_array());
  REQUIRE(cold.size() == 3);
  for (const Json& n : cold) {
    CHECK(n["cached"] == false);
    CHECK(n["cacheKey"].get<std::string>().size() == 32);
  }
  CHECK(cold[0]["level"] == 0);
  CHECK(cold[2]["level"] == 2);
  CHECK(cold[0]["upstreamMissing"] == false);  // 没有上游
  CHECK(cold[1]["upstreamMissing"] == true);

  runGraphCached(doc);

  const Json warm = Json::parse(exec::planGraphJson(doc.dump(), {}, {}));
  std::set<std::string> predicted;
  for (const Json& n : warm) {
    if (n["cached"].get<bool>()) predicted.insert(n["nodeId"].get<std::string>());
  }
  const RunLog again = runGraphCached(doc);
  CHECK(predicted == statesOf(again, "skipped"));
  CHECK(predicted.size() == 3);
}

TEST_CASE("lyflow_plan 校验失败时返回的是诊断而不是计划") {
  ensureTestOps();
  const Json doc = makeGraph({{"a", "no.such.op"}}, {});
  const Json out = Json::parse(exec::planGraphJson(doc.dump(), {}, {}));
  REQUIRE(out.is_array());
  REQUIRE(out.size() >= 1);
  CHECK(out[0]["kind"] == "diagnostic");
  CHECK(out[0]["code"] == "unknown_op");
}

TEST_CASE("LRU 字节预算：超预算时从最久没用的一头淘汰") {
  ensureTestOps();
  exec::ResultStore& store = exec::ResultStore::instance();
  store.clear();
  const std::uint64_t original = store.budget();

  // 先在大预算下攒三份互不相同的结果
  store.setBudget(64ull * 1024 * 1024);
  for (int i = 0; i < 3; ++i) {
    runGraphCached(makeGraph({{"g", "gen.synthetic", Json{{"pointCount", 20000}, {"seed", i}}}}, {}));
  }
  const auto before = store.stats();
  CHECK(before.entries == 3);
  CHECK(before.bytes > 0);

  // 预算压到一份的大小，多出来的必须被淘汰掉
  store.setBudget(before.bytes / 3 + 1);
  const auto after = store.stats();
  CHECK(after.entries < before.entries);
  CHECK(after.bytes <= after.budgetBytes);
  CHECK(after.evictions > 0);

  store.setBudget(0);
  CHECK(store.budget() == original);
  store.clear();
  CHECK(store.stats().entries == 0);
}

TEST_CASE("纯副作用算子不参与缓存：没有输出端口就永远不 skipped") {
  ensureTestOps();
  const auto dir = std::filesystem::temp_directory_path() / "lyflow-cache-sink";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 500}}},
          {"w", "test.sink", Json{{"path", "out.pcd"}}},
      },
      {{"g.cloud", "w.cloud"}});

  REQUIRE(runGraph(doc, dir).runStatus() == "ok");
  std::filesystem::remove(dir / "out.pcd");

  const RunLog second = runGraphCached(doc, dir);
  CHECK(second.runStatus() == "ok");
  CHECK(second.finalState("g") == "skipped");
  CHECK(second.finalState("w") == "done");
  // 关键：文件真的又写了一遍。存盘节点被跳过是最坏的一类「优化」。
  CHECK(std::filesystem::exists(dir / "out.pcd"));
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------- 1.2 并行

TEST_CASE("菱形图：四支各睡 200 ms，并行墙钟 < 500 ms") {
  ensureTestOps();
  const Json doc = diamondGraph(200);

  Session parallel(doc);
  RunLog& log = parallel.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(log.seqIsDense());
  CHECK(runDurationMs(log) < 500.0);
  const Json started = log.ofKind("run_started").front();
  CHECK(started["maxParallel"].get<int>() >= 2);
}

TEST_CASE("同一张图串行跑要慢得多 —— 证明快的那次真的是并行") {
  ensureTestOps();
  Session serial(diamondGraph(120), {}, {}, /*keepCache=*/false, /*maxParallel=*/1);
  RunLog& log = serial.wait();

  CHECK(log.runStatus() == "ok");
  CHECK(runDurationMs(log) >= 4 * 120.0);
}

TEST_CASE("并行下事件 seq 无重复无空洞") {
  ensureTestOps();
  const RunLog log = runGraph(diamondGraph(20));
  REQUIRE(log.runStatus() == "ok");

  std::set<std::int64_t> seen;
  for (const Json& e : log.events) {
    const auto seq = e.value("seq", std::int64_t{-1});
    CHECK(seen.insert(seq).second);  // 无重复
  }
  CHECK(log.seqIsDense());  // 无空洞
  // 每个节点都恰好走一遍 pending → running → done
  for (const std::string& id : {"s0", "s1", "s2", "s3"}) {
    CHECK(log.finalState(id) == "done");
  }
}

TEST_CASE("随机取消 100 次：不死锁、不泄漏、状态始终自洽") {
  ensureTestOps();
  const Json doc = diamondGraph(60);
  std::mt19937 rng(20260906);
  std::uniform_int_distribution<int> delay(0, 40);

  for (int i = 0; i < 100; ++i) {
    Session s(doc);
    std::this_thread::sleep_for(Ms(delay(rng)));
    s.run().cancel();
    // join 挂住的话整个测试进程会停在这里 —— 死锁的表现就是它永远回不来
    RunLog& log = s.wait();
    const std::string status = log.runStatus();
    CHECK((status == "cancelled" || status == "ok"));
    CHECK(log.seqIsDense());
    for (const Json& e : log.ofKind("node_state")) {
      CHECK(e.value("state", "") != "error");
    }
  }
  // 每个 Session 析构都走 freeRun；索引清干净了，缓存条目也没有无限长
  CHECK(exec::ResultStore::instance().stats().entries < 200);
  exec::ResultStore::instance().clear();
}

// ------------------------------------------------------ 1.3 bypass 与 Any

TEST_CASE("bypass：静音的抽稀节点让下游拿到原始点数") {
  ensureTestOps();
  Json doc = chainGraph();
  doc["nodes"][1]["bypass"] = true;
  const RunLog log = runGraph(doc);

  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("v") == "skipped");
  const Json v = log.nodeEvent("v", "skipped");
  CHECK(v["stats"]["bypassed"] == true);
  CHECK(v["stats"].contains("cached") == false);

  const Json g = log.nodeEvent("g", "done");
  const Json p = log.nodeEvent("p", "done");
  // 抽稀没跑，所以下游看到的就是源头那个点数
  CHECK(v["stats"]["elementCount"] == g["stats"]["elementCount"]);
  CHECK(p["stats"]["elementCount"] == g["stats"]["elementCount"]);
}

TEST_CASE("bypass 改变 cacheKey：取消静音之后不会拿到静音时的结果") {
  ensureTestOps();
  Json muted = chainGraph();
  muted["nodes"][1]["bypass"] = true;
  const RunLog a = runGraph(muted);
  REQUIRE(a.runStatus() == "ok");

  const RunLog b = runGraphCached(chainGraph());
  CHECK(b.runStatus() == "ok");
  CHECK(b.finalState("g") == "skipped");  // 源头没变
  CHECK(b.finalState("v") == "done");     // 静音状态变了，必须重算
  CHECK(b.nodeEvent("v", "done")["stats"]["elementCount"].get<int>() <
        a.nodeEvent("v", "skipped")["stats"]["elementCount"].get<int>());
}

TEST_CASE("bypass 找不到类型兼容的源：下游报 bypassed_no_source") {
  ensureTestOps();
  // split 吃 cloud + indices 出 selected/rest，静音后 indices
  // 那一路没有 Indices 类型的输入可以透传。
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"pl", "test.half_indices"},
          {"x", "test.split"},
          {"tail", "test.thin"},
      },
      {{"g.cloud", "pl.cloud"},
       {"g.cloud", "x.cloud"},
       {"pl.indices", "x.indices"},
       {"x.rest", "tail.cloud"}});
  Json muted = doc;
  muted["nodes"][1]["bypass"] = true;  // 静音 half_indices

  const RunLog log = runGraph(muted);
  CHECK(log.runStatus() == "error");
  CHECK(log.finalState("pl") == "skipped");
  CHECK(log.finalState("x") == "error");
  const Json x = log.nodeEvent("x", "error");
  CHECK(x["errors"][0]["code"] == "bypassed_no_source");
  CHECK(x["errors"][0]["portName"] == "indices");
}

TEST_CASE("Any 推导：reroute 串两级后端口是 PointCloud，接错类型被拒") {
  ensureTestOps();
  const Json ok = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"r1", "util.reroute"},
          {"r2", "util.reroute"},
          {"p", "test.thin"},
      },
      {{"g.cloud", "r1.in"}, {"r1.out", "r2.in"}, {"r2.out", "p.cloud"}});

  exec::RawGraph raw;
  Diagnostics diags;
  REQUIRE(exec::parseGraph(ok.dump(), raw, diags));
  exec::Plan plan;
  exec::BuildOptions options;
  options.runId = "any-test";
  REQUIRE(exec::buildPlan(ensureRegistry(), raw, options, plan, diags));
  CHECK_FALSE(diags.hasErrors());
  for (const auto& n : plan.nodes) {
    if (n.id != "r1" && n.id != "r2") continue;
    CHECK(n.inputTypes.at("in") == "PointCloud");
    CHECK(n.outputTypes.at("out") == "PointCloud");
  }

  const RunLog log = runGraph(ok);
  CHECK(log.runStatus() == "ok");
  CHECK(log.nodeEvent("r2", "done")["stats"]["outputs"][0]["type"] == "PointCloud");

  // 推导出 PointCloud 之后再往 Indices 端口上接就该被拒
  const Json bad = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"r", "util.reroute"},
          {"x", "test.split"},
      },
      {{"g.cloud", "r.in"}, {"g.cloud", "x.cloud"}, {"r.out", "x.indices"}});
  Diagnostics badDiags;
  exec::RawGraph badRaw;
  REQUIRE(exec::parseGraph(bad.dump(), badRaw, badDiags));
  exec::Plan badPlan;
  exec::buildPlan(ensureRegistry(), badRaw, options, badPlan, badDiags);
  bool sawMismatch = false;
  for (const auto& d : badDiags.items()) {
    if (d.status.code == "type_mismatch") sawMismatch = true;
  }
  CHECK(sawMismatch);
}

TEST_CASE("孤立的 reroute 推不出类型也不报错") {
  ensureTestOps();
  const Json doc = makeGraph({{"r", "util.reroute"}}, {});
  Diagnostics diags;
  exec::RawGraph raw;
  REQUIRE(exec::parseGraph(doc.dump(), raw, diags));
  exec::Plan plan;
  exec::BuildOptions options;
  options.runId = "lonely";
  exec::buildPlan(ensureRegistry(), raw, options, plan, diags);
  // 必填输入没连是它自己的错，但绝不该多出一条 type_mismatch
  for (const auto& d : diags.items()) {
    CHECK(d.status.code != "type_mismatch");
  }
}

// ---------------------------------------------------------------- 1.4 迁移

TEST_CASE("迁移链：v1 的图产出 migration 诊断") {
  ensureTestOps();
  Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"s", "test.migrated", Json{{"count", 123}, {"seed", 9}}},
      },
      {{"g.cloud", "s.cloud"}});
  doc["nodes"][1]["opVersion"] = "1.0.0";

  const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
  REQUIRE(diags.is_array());
  Json migration;
  for (const Json& d : diags) {
    if (d.value("kind", "") == "migration") migration = d;
  }
  REQUIRE_FALSE(migration.is_null());
  CHECK(migration["nodeId"] == "s");
  CHECK(migration["severity"] == "warning");
  CHECK(migration["op"] == "test.migrated");
  CHECK(migration["opVersion"] == "2.0.0");
  CHECK(migration["params"]["keepCount"] == 123);
  CHECK(migration["params"].contains("count") == false);
  CHECK(migration["params"]["seed"] == 9);
  CHECK(migration["notes"].size() >= 1);

  // 没有别的错：迁移之后的参数是合法的，执行器就用它跑
  for (const Json& d : diags) {
    CHECK(d.value("severity", "") != "error");
  }
}

TEST_CASE("迁移是在内存里生效的：老图直接跑得通") {
  ensureTestOps();
  Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"s", "test.migrated", Json{{"count", 100}}},
      },
      {{"g.cloud", "s.cloud"}});
  doc["nodes"][1]["opVersion"] = "1.0.0";

  const RunLog log = runGraph(doc);
  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("s") == "done");
  CHECK(log.nodeEvent("s", "done")["stats"]["elementCount"] == 100);
}

TEST_CASE("迁移之后再存再开：不再产出迁移诊断") {
  ensureTestOps();
  Json doc = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"s", "test.migrated", Json{{"keepCount", 123}, {"seed", 9}}},
      },
      {{"g.cloud", "s.cloud"}});
  doc["nodes"][1]["opVersion"] = "2.0.0";
  const Json diags = Json::parse(exec::validateGraphJson(doc.dump(), {}));
  for (const Json& d : diags) {
    CHECK(d.value("kind", "") != "migration");
  }
}

TEST_CASE("别名重定向也是一次迁移") {
  ensureTestOps();
  Registry r;
  OperatorDesc op;
  op.id = "test.renamed";
  op.version = "1.0.0";
  op.label = "Renamed";
  op.category = "Test";
  op.aliases = {"test.old_name"};
  op.outputs = {Port{"out", "Transform", "Out", "", true}};
  op.compute = [](const Inputs&, const ParamView&, Outputs& o, ExecContext&) {
    o.set("out", Data::transform(Transform{}));
    return Status::Ok();
  };
  r.addType(PortType{"Transform", "#a0d030", {}, ""});
  r.addOperator(std::move(op));
  CHECK(r.validate().empty());

  const Json doc = makeGraph({{"n", "test.old_name"}}, {});
  exec::RawGraph raw;
  Diagnostics diags;
  REQUIRE(exec::parseGraph(doc.dump(), raw, diags));
  exec::Plan plan;
  exec::BuildOptions options;
  options.runId = "alias";
  exec::buildPlan(r, raw, options, plan, diags);

  const Json out = Json::parse(diags.toJson());
  REQUIRE(out.size() == 1);
  CHECK(out[0]["kind"] == "migration");
  CHECK(out[0]["op"] == "test.renamed");
}

TEST_CASE("注册表自检：迁移链断档会被挡在启动时") {
  ensureTestOps();
  auto makeOp = [](const char* version, std::vector<Migration> migrations) {
    OperatorDesc op;
    op.id = "test.chain";
    op.version = version;
    op.label = "Chain";
    op.category = "Test";
    op.outputs = {Port{"out", "Transform", "Out", "", true}};
    op.compute = [](const Inputs&, const ParamView&, Outputs& o, ExecContext&) {
      o.set("out", Data::transform(Transform{}));
      return Status::Ok();
    };
    op.migrations = std::move(migrations);
    return op;
  };
  auto identity = [](const nlohmann::json& p) { return p; };

  Registry broken;
  broken.addType(PortType{"Transform", "#a0d030", {}, ""});
  broken.addOperator(makeOp("3.0.0", {Migration{1, identity}}));  // 缺 2 → 3
  const auto problems = broken.validate();
  REQUIRE_FALSE(problems.empty());
  CHECK(problems.front().find("migration from major 2") != std::string::npos);

  Registry whole;
  whole.addType(PortType{"Transform", "#a0d030", {}, ""});
  whole.addOperator(makeOp("3.0.0", {Migration{1, identity}, Migration{2, identity}}));
  CHECK(whole.validate().empty());
}

// ------------------------------------------------------------- 1.6 参数联动

TEST_CASE("隐藏的 path 参数不校验必填") {
  ensureTestOps();
  // test.sink 的 path 是可见的必填项，空着就该红
  const Json empty = makeGraph(
      {
          {"g", "gen.synthetic", kSmall},
          {"w", "test.sink", Json{{"path", ""}}},
      },
      {{"g.cloud", "w.cloud"}});
  const Json diags = Json::parse(exec::validateGraphJson(empty.dump(), {}));
  bool sawPathError = false;
  for (const Json& d : diags) {
    if (d.value("paramPath", "") == "path" && d.value("severity", "") == "error") {
      sawPathError = true;
    }
  }
  CHECK(sawPathError);
}

TEST_CASE("被 visibleWhen 藏起来的参数只查形态不查必填") {
  ensureTestOps();
  Registry r;
  r.addType(PortType{"Transform", "#a0d030", {}, ""});

  OperatorDesc op;
  op.id = "test.hidden_path";
  op.version = "1.0.0";
  op.label = "Hidden Path";
  op.category = "Test";
  op.outputs = {Port{"out", "Transform", "Out", "", true}};
  op.compute = [](const Inputs&, const ParamView&, Outputs& o, ExecContext&) {
    o.set("out", Data::transform(Transform{}));
    return Status::Ok();
  };

  Param source;
  source.name = "source";
  source.type = ParamType::Enum;
  source.label = "Source";
  source.def = Value::text("identity");
  source.options = {EnumOption{"identity", "Identity", ""}, EnumOption{"file", "File", ""}};

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "Path";
  path.def = Value::text("");
  path.visibleWhen.param = "source";
  path.visibleWhen.eq = Value::text("file");

  op.params = {source, path};
  r.addOperator(std::move(op));
  REQUIRE(r.validate().empty());

  auto diagnose = [&](const Json& params) {
    Json doc = makeGraph({{"n", "test.hidden_path", params}}, {});
    exec::RawGraph raw;
    Diagnostics diags;
    REQUIRE(exec::parseGraph(doc.dump(), raw, diags));
    exec::Plan plan;
    exec::BuildOptions options;
    options.runId = "hidden";
    exec::buildPlan(r, raw, options, plan, diags);
    return Json::parse(diags.toJson());
  };

  // 藏起来时不报「还没有选择文件」—— 用户根本没有那个输入框可以填
  CHECK(diagnose(Json::object()).empty());
  // 露出来时照常报
  const Json shown = diagnose(Json{{"source", "file"}});
  REQUIRE(shown.size() == 1);
  CHECK(shown[0]["paramPath"] == "path");
  // 形态照查：藏着也不许是数字
  const Json wrongShape = diagnose(Json{{"path", 42}});
  REQUIRE(wrongShape.size() == 1);
  CHECK(wrongShape[0]["code"] == "bad_param");
}
