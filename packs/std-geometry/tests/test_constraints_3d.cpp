#include <doctest/doctest.h>

#include "helpers.h"

namespace lyflow::packs::std_geometry {
void registerConstraints3D(Registry&);
}

namespace {
using namespace lyflow;
using Json = nlohmann::json;

const Registry& constraintsRegistry() {
  static Registry registry;
  static const bool ready = [] {
    packs::std_geometry::registerConstraints3D(registry);
    return true;
  }();
  (void)ready;
  return registry;
}

Json constraintItems() {
  return Json::array({{{"id", "point"},
                       {"type", "point"},
                       {"source", {0, 0, 0}},
                       {"target", {1, 2, 3}},
                       {"weight", 8}}});
}

class CancelConstruction final : public ExecContext {
public:
  explicit CancelConstruction(unsigned poll) : cancelAt(poll) {}
  bool cancelled() const override { return ++polls >= cancelAt; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 1; }
  mutable unsigned polls = 0;
  unsigned cancelAt;
  std::filesystem::path base;
};
} // namespace

TEST_CASE("Geometry owns the unchanged three dimensional constraints constructor contract") {
  const auto* op = constraintsRegistry().find("register.constraints_3d");
  REQUIRE(op != nullptr);
  CHECK(op->version == "1.0.0");
  CHECK(op->label == "三维几何约束构造");
  CHECK(op->category == "点云/三维分析");
  CHECK(op->inputs.empty());
  REQUIRE(op->outputs.size() == 1);
  CHECK(op->outputs[0].name == "constraints");
  CHECK(op->outputs[0].type == "Record");
  CHECK(op->outputs[0].contract == Json{{"recordType", "cloud.Constraints3D.v1"}});
  REQUIRE(op->params.size() == 4);
  CHECK(op->params[0].name == "sourceFrame");
  CHECK(op->params[1].name == "targetFrame");
  CHECK(op->params[2].name == "mode");
  CHECK(op->params[3].name == "items");
  CHECK(op->capabilities.cancellable);
  CHECK(op->capabilities.deterministic);
  test::OpCall call(constraintsRegistry());
  const Json items = constraintItems();
  REQUIRE(call.run(op->id, {{"items", Value::text(items.dump())}}).ok);
  REQUIRE(call.out("constraints").asRecord() != nullptr);
  CHECK(call.out("constraints").asRecord()->type == "cloud.Constraints3D.v1");
  CHECK(
      call.out("constraints").asRecord()->data ==
      Json{{"sourceFrame", "source"}, {"targetFrame", "target"}, {"unit", "m"}, {"items", items}});
  for (const std::string bad : {std::string("["), std::string("[]"), std::string("{}"),
                                std::string(65537, ' '), std::string("[{}]")}) {
    CHECK_FALSE(call.run(op->id, {{"items", Value::text(bad)}}).ok);
    CHECK(call.status.code == "bad_param");
    CHECK(call.status.paramPath == "items");
    CHECK(call.outputs.empty());
  }
  CHECK_FALSE(
      call.run(op->id, {{"items", Value::text(items.dump())}, {"sourceFrame", Value::text("")}})
          .ok);
  CHECK(call.status.paramPath == "sourceFrame");
}

TEST_CASE("Geometry constraints construction cancellation never publishes partial records") {
  const auto* op = constraintsRegistry().find("register.constraints_3d");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& param : op->params)
    params[param.name] = param.def;
  params["items"] = Value::text(constraintItems().dump());
  const std::unordered_map<std::string, Data> values;
  for (const unsigned poll : {1u, 2u}) {
    CancelConstruction ctx(poll);
    std::unordered_map<std::string, Data> result;
    Outputs output(result);
    const Status status = op->compute(Inputs(values), ParamView(params, ctx.base), output, ctx);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "cancelled");
    CHECK(status.portName == "items");
    CHECK(result.empty());
  }
}
