// 校验与编译：D5（一次返回全部诊断）、默认值合并、类型兼容、环检测、cacheKey。
#include <doctest/doctest.h>

#include <algorithm>

#include "exec/graph.h"
#include "exec/plan.h"
#include "helpers.h"
#include "lyflow/registry.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

struct Built {
  Diagnostics diags;
  exec::Plan plan;
  bool parsed = false;
  bool built = false;

  bool hasCode(const std::string& nodeId, const std::string& code) const {
    for (const auto& d : diags.items()) {
      if (d.nodeId == nodeId && d.status.code == code) return true;
    }
    return false;
  }
  const Diagnostic* find(const std::string& nodeId, const std::string& code) const {
    for (const auto& d : diags.items()) {
      if (d.nodeId == nodeId && d.status.code == code) return &d;
    }
    return nullptr;
  }
  std::size_t errorCount() const {
    std::size_t n = 0;
    for (const auto& d : diags.items()) {
      if (d.severity == Severity::Error) ++n;
    }
    return n;
  }
};

Built build(const Json& doc, const std::vector<std::string>& targets = {}) {
  Built out;
  exec::RawGraph raw;
  out.parsed = exec::parseGraph(doc.dump(), raw, out.diags);
  if (!out.parsed) return out;
  exec::BuildOptions options;
  options.runId = "plan-test";
  options.targets = targets;
  out.built = exec::buildPlan(ensureRegistry(), raw, options, out.plan, out.diags);
  return out;
}

const exec::PlanNode* nodeOf(const exec::Plan& plan, const std::string& id) {
  for (const auto& n : plan.nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("注册表自检干净，15 个算子都在") {
  const auto problems = ensureRegistry().validate();
  for (const auto& p : problems) MESSAGE(p);
  CHECK(problems.empty());

  const std::vector<std::string> expected = {
      "gen.synthetic",       "io.load_pcd",
      "io.save_pcd",         "filter.passthrough",
      "filter.voxel_grid",   "filter.crop_box",
      "filter.random_sample", "filter.statistical_outlier",
      "filter.radius_outlier", "features.normals",
      "segment.ransac_plane", "segment.extract_indices",
      "transform.make",      "transform.apply",
      "util.merge",
  };
  for (const auto& id : expected) {
    CAPTURE(id);
    CHECK(ensureRegistry().find(id) != nullptr);
  }
  // D10：PointCloudXYZI 已从类型表删除
  CHECK(ensureRegistry().findType("PointCloudXYZI") == nullptr);
  CHECK(ensureRegistry().findType("Plane") != nullptr);
}

TEST_CASE("拓扑序：上游一定排在下游之前") {
  const Json doc = makeGraph(
      {
          {"c", "filter.voxel_grid"},
          {"a", "gen.synthetic"},
          {"b", "filter.passthrough"},
      },
      {{"a.cloud", "b.cloud"}, {"b.cloud", "c.cloud"}});

  const Built r = build(doc);
  REQUIRE(r.built);
  REQUIRE(r.plan.nodes.size() == 3);
  CHECK(r.plan.nodes[0].id == "a");
  CHECK(r.plan.nodes[1].id == "b");
  CHECK(r.plan.nodes[2].id == "c");
  // level 是 M3 并行执行的依据，M2 就得算对
  CHECK(r.plan.nodes[0].level == 0);
  CHECK(r.plan.nodes[2].level == 2);
}

TEST_CASE("环：环上每个节点各一条 cycle 诊断，整图级失败") {
  // b 用 util.merge（两个输入端口）：换成单输入算子的话，「a 和 c 都连到
  // b.cloud」会先被单连接规则挡下来，测到的就不是环检测了。
  const Json doc = makeGraph(
      {
          {"a", "gen.synthetic"},
          {"b", "util.merge"},
          {"c", "filter.voxel_grid"},
      },
      {{"a.cloud", "b.a"}, {"b.cloud", "c.cloud"}, {"c.cloud", "b.b"}});

  const Built r = build(doc);
  CHECK_FALSE(r.built);
  CHECK(r.hasCode("b", "cycle"));
  CHECK(r.hasCode("c", "cycle"));
  // a 不在环上，不该被牵连
  CHECK_FALSE(r.hasCode("a", "cycle"));
}

TEST_CASE("D5：一次返回全部诊断，不在第一个错误处早退") {
  const Json doc = makeGraph(
      {
          {"a", "gen.synthetic", Json{{"pointCount", -5}, {"noise", "字符串不是数字"}}},
          {"b", "filter.voxel_grid", Json{{"leafSize", 0.01}, {"nonexistent", 1}}},
          {"c", "no.such.operator"},
      },
      {{"a.cloud", "b.cloud"}});

  const Built r = build(doc);
  REQUIRE(r.built);  // 节点级错误不阻止整图编译

  // a：pointCount 越界 + noise 类型不对，两条都要在
  const Diagnostic* count = r.find("a", "bad_param");
  REQUIRE(count != nullptr);
  std::size_t aErrors = 0;
  std::vector<std::string> aParams;
  for (const auto& d : r.diags.items()) {
    if (d.nodeId == "a" && d.severity == Severity::Error) {
      ++aErrors;
      aParams.push_back(d.status.paramPath);
    }
  }
  CHECK(aErrors == 2);
  CHECK(std::find(aParams.begin(), aParams.end(), "pointCount") != aParams.end());
  CHECK(std::find(aParams.begin(), aParams.end(), "noise") != aParams.end());

  // b：leafSize 是标量而不是长度 3 的数组，外加一个未知参数名
  CHECK(r.hasCode("b", "bad_param"));
  const Diagnostic* unknown = r.find("b", "unknown_param");
  REQUIRE(unknown != nullptr);
  CHECK(unknown->status.paramPath == "nonexistent");

  // c：算子不存在
  CHECK(r.hasCode("c", "unknown_op"));

  // 三个节点各自被标成无效，但计划照样排得出来 —— 其余节点还要跑
  CHECK(nodeOf(r.plan, "a")->valid == false);
  CHECK(nodeOf(r.plan, "b")->valid == false);
}

TEST_CASE("paramPath 精确指向出错的参数框") {
  const Json doc = makeGraph({{"v", "filter.voxel_grid", Json{{"leafSize", {0.0, 0.01, 0.01}}}}}, {});
  const Built r = build(doc);
  const Diagnostic* d = r.find("v", "bad_param");
  REQUIRE(d != nullptr);
  CHECK(d->status.paramPath == "leafSize");
  CHECK(std::string(toString(d->status.phase)) == "validate");
}

TEST_CASE("默认值合并：没写的参数用 manifest 的默认值") {
  const Json doc = makeGraph({{"g", "gen.synthetic", Json{{"seed", 7}}}}, {});
  const Built r = build(doc);
  REQUIRE(r.built);
  const exec::PlanNode* g = nodeOf(r.plan, "g");
  REQUIRE(g != nullptr);
  CHECK(g->params.at("seed").intValue() == 7);
  CHECK(g->params.at("pointCount").intValue() == 40000);  // manifest 默认值
  CHECK(g->params.at("withIntensity").boolValue() == true);
  // 稀疏存储的含义：图里没写的键在这里必须存在，算子才敢无条件取值
  CHECK(g->params.size() == ensureRegistry().find("gen.synthetic")->params.size());
}

TEST_CASE("类型兼容矩阵") {
  // 相等：PointCloud → PointCloud
  {
    const Json doc = makeGraph({{"a", "gen.synthetic"}, {"b", "filter.passthrough"}},
                               {{"a.cloud", "b.cloud"}});
    CHECK(build(doc).errorCount() == 0);
  }
  // 不等：Indices → PointCloud 端口
  {
    const Json doc = makeGraph(
        {
            {"a", "gen.synthetic"},
            {"s", "segment.ransac_plane"},
            {"b", "filter.passthrough"},
        },
        {{"a.cloud", "s.cloud"}, {"s.inliers", "b.cloud"}});
    const Built r = build(doc);
    CHECK(r.hasCode("b", "type_mismatch"));
  }
  // 端口不存在
  {
    const Json doc =
        makeGraph({{"a", "gen.synthetic"}, {"b", "filter.passthrough"}}, {{"a.nope", "b.cloud"}});
    CHECK(build(doc).hasCode("a", "unknown_port"));
  }
  // 必填输入没连
  {
    const Json doc = makeGraph({{"b", "filter.passthrough"}}, {});
    const Built r = build(doc);
    const Diagnostic* d = r.find("b", "missing_input");
    REQUIRE(d != nullptr);
    CHECK(d->status.portName == "cloud");
  }
  // 可选输入没连：crop_box 的 pose 是 required=false，不该报
  {
    const Json doc = makeGraph({{"a", "gen.synthetic"}, {"c", "filter.crop_box"}},
                               {{"a.cloud", "c.cloud"}});
    CHECK(build(doc).errorCount() == 0);
  }
}

TEST_CASE("path 参数为空在校验期就红，而不是等到执行时报打不开空文件名") {
  const Json doc = makeGraph({{"l", "io.load_pcd"}}, {});
  const Built r = build(doc);
  const Diagnostic* d = r.find("l", "bad_param");
  REQUIRE(d != nullptr);
  CHECK(d->status.paramPath == "path");
}

TEST_CASE("结构性问题也是诊断，不是异常") {
  Diagnostics diags;
  exec::RawGraph raw;
  // 边指向不存在的节点
  Json doc = makeGraph({{"a", "gen.synthetic"}}, {});
  doc["edges"].push_back(Json{{"id", "e9"},
                              {"from", {{"node", "a"}, {"port", "cloud"}}},
                              {"to", {{"node", "ghost"}, {"port", "cloud"}}}});
  CHECK_FALSE(exec::parseGraph(doc.dump(), raw, diags));
  bool found = false;
  for (const auto& d : diags.items()) {
    if (d.status.code == "unknown_node") found = true;
  }
  CHECK(found);
}

TEST_CASE("输入端口是单连接") {
  Json doc = makeGraph({{"a", "gen.synthetic"}, {"b", "gen.synthetic"}, {"m", "util.merge"}},
                       {{"a.cloud", "m.a"}, {"b.cloud", "m.a"}});
  Diagnostics diags;
  exec::RawGraph raw;
  CHECK_FALSE(exec::parseGraph(doc.dump(), raw, diags));
  bool found = false;
  for (const auto& d : diags.items()) {
    if (d.status.code == "multi_input" && d.status.portName == "a") found = true;
  }
  CHECK(found);
}

TEST_CASE("cacheKey：参数变则键变，书写顺序变则键不变") {
  const Json a = makeGraph({{"g", "gen.synthetic", Json{{"seed", 1}, {"pointCount", 100}}}}, {});
  const Json b = makeGraph({{"g", "gen.synthetic", Json{{"pointCount", 100}, {"seed", 1}}}}, {});
  const Json c = makeGraph({{"g", "gen.synthetic", Json{{"seed", 2}, {"pointCount", 100}}}}, {});

  const std::string ka = nodeOf(build(a).plan, "g")->cacheKey;
  const std::string kb = nodeOf(build(b).plan, "g")->cacheKey;
  const std::string kc = nodeOf(build(c).plan, "g")->cacheKey;

  CHECK(ka.size() == 32);
  CHECK(ka == kb);
  CHECK(ka != kc);
}

TEST_CASE("cacheKey 沿着依赖链传播") {
  auto keyOf = [](int seed) {
    const Json doc = makeGraph(
        {{"g", "gen.synthetic", Json{{"seed", seed}}}, {"v", "filter.voxel_grid"}},
        {{"g.cloud", "v.cloud"}});
    return nodeOf(build(doc).plan, "v")->cacheKey;
  };
  CHECK(keyOf(1) != keyOf(2));  // 上游参数变了，下游的键必须跟着变
  CHECK(keyOf(1) == keyOf(1));
}

TEST_CASE("Run to node 只保留目标的上游闭包") {
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic"},
          {"v", "filter.voxel_grid"},
          {"p", "filter.passthrough"},
      },
      {{"g.cloud", "v.cloud"}, {"v.cloud", "p.cloud"}});

  const Built r = build(doc, {"v"});
  REQUIRE(r.built);
  CHECK(r.plan.nodes.size() == 2);
  CHECK(nodeOf(r.plan, "p") == nullptr);
  CHECK(nodeOf(r.plan, "g") != nullptr);
}

TEST_CASE("canonicalParamsJson 键排序且数字稳定") {
  ParamMap m;
  m["z"] = Value::integer(1);
  m["a"] = Value::number(2.0);
  m["m"] = Value::text("x");
  const std::string json = exec::canonicalParamsJson(m);
  CHECK(json == "{\"a\":2.0,\"m\":\"x\",\"z\":1}");
}
