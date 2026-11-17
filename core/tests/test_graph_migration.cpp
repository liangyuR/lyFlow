#include "helpers.h"
#include "exec/plan.h"
#include "lyflow/graph_migration.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {
void renameOp(Json& level, const Json& rule, Json* outer) {
  const auto id = graph_migration::text(rule.at("target"), "node");
  auto& nodes = level["nodes"];
  const auto i = graph_migration::nodeIndex(nodes, id);
  graph_migration::require(i < nodes.size(), "目标节点不存在: " + id);
  nodes[i]["op"] = graph_migration::text(rule, "op");
  nodes[i]["params"]["outer"] = outer != nullptr;
}

Json renameRule(const std::string& node, const std::string& op) {
  return {{"kind", "test.rename"}, {"target", {{"node", node}, {"port", "in"}}}, {"op", op}};
}

Json request(Json rules) { return {{"schemaVersion", 1}, {"rules", std::move(rules)}}; }

void addRename(Registry& registry) { registry.addGraphMigration({"test.rename", {}, renameOp}); }
} // namespace

TEST_CASE("Graph migration dispatches rules by kind in order and only strips contractUpgrade") {
  Registry registry;
  addRename(registry);
  auto graph = makeGraph({{"a", "old.op"}, {"b", "old.op"}}, {{"a.out", "b.in"}});
  graph["meta"] = {{"owner", "keep"}};
  graph["contractUpgrade"] = {{"legacy", true}};
  Json migrated;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrateGraph(registry, graph,
                               request(Json::array(
                                   {renameRule("b", "first.op"), renameRule("b", "second.op")})),
                               migrated, error),
                  error.message);
  CHECK(error.nodeId.empty());
  CHECK(error.message.empty());
  auto expected = graph;
  expected.erase("contractUpgrade");
  expected["nodes"][1]["op"] = "second.op";
  expected["nodes"][1]["params"]["outer"] = false;
  CHECK(migrated == expected);

  REQUIRE(migrateGraph(registry, graph, request(Json::array()), migrated, error));
  expected = graph;
  expected.erase("contractUpgrade");
  CHECK(migrated == expected);

  Registry invalid;
  invalid.addGraphMigration({"", {}, nullptr});
  CHECK_FALSE(invalid.validate().empty());
}

TEST_CASE("Graph migration reports the rule's node and rejects malformed requests") {
  Registry registry;
  addRename(registry);
  const auto graph = makeGraph({{"a", "old.op"}, {"b", "old.op"}}, {{"a.out", "b.in"}});
  Json migrated;
  GraphMigrationError error;

  auto unknown = renameRule("b", "new.op");
  unknown["kind"] = "test.missing";
  CHECK_FALSE(migrateGraph(registry, graph, request(Json::array({unknown})), migrated, error));
  CHECK(error.nodeId == "b");
  CHECK(error.message.find("test.missing") != std::string::npos);

  unknown["source"] = {{"node", "a"}, {"port", "out"}};
  CHECK_FALSE(migrateGraph(registry, graph, request(Json::array({unknown})), migrated, error));
  CHECK(error.nodeId == "a");

  CHECK_FALSE(
      migrateGraph(registry, graph,
                   request(Json::array({renameRule("a", "new.op"), renameRule("ghost", "new.op")})),
                   migrated, error));
  CHECK(error.nodeId == "ghost");
  CHECK(error.message.find("ghost") != std::string::npos);

  for (const auto& bad :
       {Json{{"schemaVersion", 2}, {"rules", Json::array()}},
        Json{{"schemaVersion", 1}, {"rules", Json::object()}}, Json{{"schemaVersion", 1}},
        Json::array(), Json{{"schemaVersion", 1}, {"rules", Json::array()}, {"extra", 1}},
        request(Json::array({Json{{"target", {{"node", "a"}}}}}))}) {
    CAPTURE(bad);
    CHECK_FALSE(migrateGraph(registry, graph, bad, migrated, error));
    CHECK_FALSE(error.message.empty());
  }
  auto noEdges = graph;
  noEdges.erase("edges");
  CHECK_FALSE(migrateGraph(registry, noEdges, request(Json::array()), migrated, error));
}

TEST_CASE("Graph migration scope routes a rule to one subgraph definition with the outer graph") {
  Registry registry;
  addRename(registry);
  const auto inner = makeGraph({{"x", "old.op"}}, {});
  Json graph{{"schemaVersion", 1},
             {"nodes", Json::array({{{"id", "x"}, {"op", "sub:inside"}},
                                    {{"id", "y"}, {"op", "sub:outside"}}})},
             {"edges", Json::array()},
             {"subgraphs", {{"inside", inner}, {"outside", inner}}}};
  auto rule = renameRule("x", "new.op");
  rule["scope"] = {{"subgraph", "inside"}};
  Json migrated;
  GraphMigrationError error;
  REQUIRE_MESSAGE(migrateGraph(registry, graph, request(Json::array({rule})), migrated, error),
                  error.message);
  CHECK(migrated["nodes"] == graph["nodes"]);
  CHECK(migrated["subgraphs"]["outside"] == inner);
  CHECK(migrated["subgraphs"]["inside"]["nodes"][0]["op"] == "new.op");
  CHECK(migrated["subgraphs"]["inside"]["nodes"][0]["params"]["outer"] == true);

  for (const auto& scope : {Json{{"subgraph", "absent"}}, Json{{"subgraph", ""}}, Json::object()}) {
    CAPTURE(scope);
    rule["scope"] = scope;
    CHECK_FALSE(migrateGraph(registry, graph, request(Json::array({rule})), migrated, error));
    CHECK(error.nodeId == "x");
  }
  rule["scope"] = {{"subgraph", "absent"}};
  CHECK_FALSE(migrateGraph(registry, graph, request(Json::array({rule})), migrated, error));
  CHECK(error.message.find("absent") != std::string::npos);
  graph["subgraphs"]["inside"].erase("edges");
  rule["scope"] = {{"subgraph", "inside"}};
  CHECK_FALSE(migrateGraph(registry, graph, request(Json::array({rule})), migrated, error));
}

TEST_CASE("Pending contractUpgrade and retired operators require offline migration") {
  for (const auto& pending : {Json(), Json::array(), request(Json::array())}) {
    CAPTURE(pending);
    auto graph = makeGraph({{"a", "old.op"}}, {});
    graph["contractUpgrade"] = pending;
    exec::RawGraph raw;
    Diagnostics diags;
    CHECK_FALSE(exec::parseGraph(graph.dump(), raw, diags));
    REQUIRE(diags.items().size() == 1);
    CHECK(diags.items()[0].nodeId.empty());
    CHECK(diags.items()[0].status.code == "migration_required");
  }
  Registry registry;
  registry.addRetiredOperator("old.retired", "运行 lyflow migrate");
  exec::RawGraph raw;
  Diagnostics diags;
  REQUIRE(exec::parseGraph(makeGraph({{"a", "old.retired"}, {"b", "old.missing"}}, {}).dump(), raw,
                           diags));
  exec::BuildOptions options;
  exec::Plan plan;
  exec::buildPlan(registry, raw, options, plan, diags);
  const auto code = [&](const char* node) {
    for (const auto& d : diags.items())
      if (d.nodeId == node)
        return d.status.code + ":" + d.status.message;
    return std::string();
  };
  CHECK(code("a").rfind("migration_required:", 0) == 0);
  CHECK(code("a").find("运行 lyflow migrate") != std::string::npos);
  CHECK(code("b").rfind("unknown_op:", 0) == 0);
}
