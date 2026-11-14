#include <doctest/doctest.h>

#include <limits>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/value.h"

namespace lyflow::test {
namespace {

Data value(double number, const std::string& unit = {}) {
  return Data::record(values::number(number, unit));
}

Data list(const Json& items, const char* kind = "number", const char* unit = "") {
  return Data::record(values::makeList(kind, items, unit));
}

} // namespace

TEST_CASE("std-value: versioned values reject malformed, unbounded and nonfinite payloads") {
  std::string why;
  CHECK(values::validate(values::number(12, "mm"), why));
  CHECK(values::validate(values::boolean(false), why));
  CHECK(values::validate(values::string("中文 UTF-8"), why));
  CHECK(values::validate(values::makeList("number", Json::array()), why));
  CHECK_FALSE(values::validate(values::number(std::numeric_limits<double>::infinity()), why));
  CHECK_FALSE(values::validate(values::makeValue("boolean", 1), why));
  CHECK_FALSE(values::validate(values::makeValue("string", "x", "m"), why));
  CHECK_FALSE(values::validate(values::makeList("number", Json::array({1, true})), why));
  CHECK_FALSE(values::validate(values::makeList("number", Json::array({nullptr})), why));
  CHECK_FALSE(values::validate(values::string(std::string(values::kMaxStringBytes + 1, 'x')), why));
  Json large = Json::array();
  for (std::size_t i = 0; i <= values::kMaxItems; ++i)
    large.push_back(i);
  CHECK_FALSE(values::validate(values::makeList("number", large), why));
  Json deep = 1;
  for (std::size_t i = 0; i <= values::kMaxDepth; ++i)
    deep = Json{{"nested", std::move(deep)}};
  CHECK_FALSE(values::boundedJson(deep, why));
  CHECK_FALSE(values::boundedJson(Json{{std::string(values::kMaxStringBytes + 1, 'k'), 1}}, why));
  CHECK_FALSE(values::validate(Record{"std.Value.v2", values::number(1).data}, why));
  auto extras = values::number(1);
  extras.data["extra"] = true;
  CHECK_FALSE(values::validate(extras, why));
}

TEST_CASE("std-value: arithmetic preserves units and rejects invalid measurements and dimensions") {
  OpCall call;
  call.inputs = {{"a", value(12, "mm")}, {"b", value(3, "mm")}};
  REQUIRE(call.run("value.arithmetic").ok);
  CHECK(call.out("value").asRecord()->data["value"] == 15);
  CHECK(call.out("value").asRecord()->data["unit"] == "mm");
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("subtract")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == 9);
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("divide")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == 4);
  CHECK(call.out("value").asRecord()->data["unit"] == "");
  CHECK_FALSE(call.run("value.arithmetic", {{"operation", Value::text("multiply")}}).ok);
  call.inputs["b"] = value(2);
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("multiply")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == 24);
  CHECK(call.out("value").asRecord()->data["unit"] == "mm");
  CHECK_FALSE(call.run("value.arithmetic").ok);
  call.inputs["b"] = value(0);
  CHECK_FALSE(call.run("value.arithmetic", {{"operation", Value::text("divide")}}).ok);
  call.inputs["b"] = value(-2);
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("divide")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == -6);
  call.inputs = {{"a", value(2)}, {"b", value(3)}};
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("power")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == 8);
  call.inputs = {{"a", value(-2)}, {"b", value(0.5)}};
  CHECK_FALSE(call.run("value.arithmetic", {{"operation", Value::text("power")}}).ok);
  call.inputs = {{"a", value(0)}, {"b", value(0)}};
  CHECK_FALSE(call.run("value.arithmetic", {{"operation", Value::text("power")}}).ok);
  call.inputs = {{"a", value(1)}, {"b", value(1e-20)}};
  REQUIRE(call.run("value.arithmetic", {{"operation", Value::text("divide")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"].get<double>() == doctest::Approx(1e20));
  call.inputs = {{"a", value(1e308)}, {"b", value(1e308)}};
  CHECK_FALSE(call.run("value.arithmetic").ok);

  Measurement m;
  m.value = 21.5;
  m.unit = "px²";
  call.inputs = {{"measurement", Data::measurement(m)}};
  CHECK_FALSE(call.run("value.from_measurement").ok);
  m.ok = true;
  call.inputs["measurement"] = Data::measurement(m);
  REQUIRE(call.run("value.from_measurement").ok);
  CHECK(call.out("value").asRecord()->data["unit"] == "px²");
  m.value = std::numeric_limits<double>::quiet_NaN();
  call.inputs["measurement"] = Data::measurement(m);
  CHECK_FALSE(call.run("value.from_measurement").ok);
  call.inputs = {{"a", Data::measurement(m)}, {"b", value(0)}};
  CHECK_FALSE(call.run("value.arithmetic").ok);
}

TEST_CASE(
    "std-value: tolerance comparisons and boolean logic have explicit type and boundary rules") {
  OpCall call;
  call.inputs = {{"a", value(10, "m")}, {"b", value(10.01, "m")}};
  const std::unordered_map<std::string, Value> tolerance{{"absoluteTolerance", Value::number(0.02)},
                                                         {"relativeTolerance", Value::number(0)}};
  REQUIRE(call.run("value.compare", tolerance).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  auto less = tolerance;
  less["operation"] = Value::text("less");
  REQUIRE(call.run("value.compare", less).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  call.inputs = {{"a", value(10)}, {"b", value(11)}};
  auto boundary = std::unordered_map<std::string, Value>{{"absoluteTolerance", Value::number(1)},
                                                         {"relativeTolerance", Value::number(0)}};
  REQUIRE(call.run("value.compare", boundary).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  boundary["operation"] = Value::text("not_equal");
  REQUIRE(call.run("value.compare", boundary).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  less["operation"] = Value::text("less_equal");
  REQUIRE(call.run("value.compare", less).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  call.inputs["b"] = value(10.01, "mm");
  CHECK_FALSE(call.run("value.compare", tolerance).ok);
  call.inputs = {{"a", value(1e308)}, {"b", value(-1e308)}};
  REQUIRE(call.run("value.compare").ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  call.inputs = {{"a", Data::record(values::boolean(true))},
                 {"b", Data::record(values::boolean(false))}};
  REQUIRE(call.run("value.logic").ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  REQUIRE(call.run("value.logic", {{"operation", Value::text("or")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  REQUIRE(call.run("value.logic", {{"operation", Value::text("xor")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  REQUIRE(call.run("value.logic", {{"operation", Value::text("nand")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  REQUIRE(call.run("value.logic", {{"operation", Value::text("nor")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  REQUIRE(call.run("value.logic", {{"operation", Value::text("xnor")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  call.inputs.erase("b");
  REQUIRE(call.run("value.logic", {{"operation", Value::text("not")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  CHECK_FALSE(call.run("value.logic").ok);
  call.inputs["a"] = value(1);
  CHECK_FALSE(call.run("value.logic", {{"operation", Value::text("not")}}).ok);
}

TEST_CASE(
    "std-value: list selection preserves duplicates, order, empty results and source values") {
  OpCall call;
  call.inputs = {{"a", value(1, "mm")}, {"b", value(2, "mm")}};
  REQUIRE(call.run("list.pack").ok);
  const Data source = call.out("list");
  CHECK(source.asRecord()->data["items"] == Json::array({1, 2}));
  call.inputs = {{"a", source}, {"b", source}};
  REQUIRE(call.run("list.concat").ok);
  CHECK(call.out("list").asRecord()->data["items"] == Json::array({1, 2, 1, 2}));
  call.inputs = {{"list", source}};
  REQUIRE(call.run("list.subset", {{"indices", Value::text("[1,0,1]")}}).ok);
  CHECK(call.out("list").asRecord()->data["items"] == Json::array({2, 1, 2}));
  REQUIRE(call.run("list.subset").ok);
  CHECK(call.out("list").asRecord()->data["items"].empty());
  CHECK_FALSE(call.run("list.subset", {{"indices", Value::text("[-1]")}}).ok);
  CHECK_FALSE(call.run("list.subset", {{"indices", Value::text("[2]")}}).ok);
  CHECK_FALSE(call.run("list.subset", {{"indices", Value::text("[0.5]")}}).ok);
  REQUIRE(call.run("list.get", {{"index", Value::integer(1)}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == 2);
  CHECK(call.out("value").asRecord()->data["unit"] == "mm");
  CHECK_FALSE(call.run("list.get", {{"index", Value::integer(-1)}}).ok);
  CHECK_FALSE(call.run("list.get", {{"index", Value::integer(2)}}).ok);
  CHECK(source.asRecord()->data["items"] == Json::array({1, 2}));
  call.inputs = {{"a", list(Json::array(), "number", "mm")}, {"b", source}};
  REQUIRE(call.run("list.concat").ok);
  CHECK(call.out("list").asRecord()->data["items"] == Json::array({1, 2}));
  call.inputs["a"] = list(Json::array(), "string");
  CHECK_FALSE(call.run("list.concat").ok);
}

TEST_CASE(
    "std-value: record list stable sort retains identities and field extraction handles null siblings") {
  const Json records =
      Json::array({Json{{"type", "test.Part.v1"}, {"data", {{"id", "a"}, {"score", 2}}}},
                   Json{{"type", "test.Part.v1"}, {"data", {{"id", "b"}, {"score", 1}}}},
                   Json{{"type", "test.Part.v1"}, {"data", {{"id", "c"}, {"score", 2}}}}});
  OpCall call;
  call.inputs = {{"list", list(records, "record")}};
  REQUIRE(call.run("list.stable_sort", {{"pointer", Value::text("/score")}}).ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({1, 0, 2}));
  const Data ordered = call.out("list");
  call.inputs = {{"list", ordered}};
  REQUIRE(call.run("list.get", {{"index", Value::integer(2)}}).ok);
  const Data part = call.out("value");
  call.inputs = {{"record", part}};
  REQUIRE(call.run("value.field", {{"pointer", Value::text("/id")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == "c");
  call.inputs = {{"list", list(records, "record")}};
  REQUIRE(call.run("list.stable_sort",
                   {{"pointer", Value::text("/score")}, {"direction", Value::text("descending")}})
              .ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({0, 2, 1}));
  CHECK_FALSE(call.run("list.stable_sort", {{"pointer", Value::text("/missing")}}).ok);
  call.inputs = {{"record", Data::record(Record{"test.Stats.v1",
                                                {{"valid", false},
                                                 {"mean", nullptr},
                                                 {"regions", Json::array({{{"area", 5}}})}}})}};
  REQUIRE(call.run("value.field", {{"pointer", Value::text("/valid")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == false);
  REQUIRE(call.run("value.field",
                   {{"pointer", Value::text("/regions/0/area")}, {"unit", Value::text("px²")}})
              .ok);
  CHECK(call.out("value").asRecord()->data["value"] == 5);
  CHECK(call.out("value").asRecord()->data["unit"] == "px²");
  CHECK_FALSE(call.run("value.field", {{"pointer", Value::text("/mean")}}).ok);
  CHECK_FALSE(call.run("value.field", {{"pointer", Value::text("/regions/2/area")}}).ok);
}

TEST_CASE("std-value: user JSON is bounded before parse and invalid constant lists fail cleanly") {
  OpCall call;
  REQUIRE(call.run("list.constant", {{"items", Value::text("[3,1,1]")}}).ok);
  CHECK(call.out("list").asRecord()->data["items"] == Json::array({3, 1, 1}));
  CHECK_FALSE(call.run("list.constant", {{"items", Value::text("[1,null]")}}).ok);
  CHECK_FALSE(call.run("list.constant", {{"items", Value::text("[1,true]")}}).ok);
  CHECK_FALSE(call.run("list.constant", {{"items", Value::text("[1,]")}}).ok);
  CHECK_FALSE(
      call.run("list.constant", {{"items", Value::text(std::string(values::kMaxBytes + 1, '['))}})
          .ok);
  CHECK_FALSE(call.run("list.constant",
                       {{"items", Value::text(std::string(40, '[') + "0" + std::string(40, ']'))}})
                  .ok);
  call.inputs = {{"list", list(Json::array({3, 1, 1}))}};
  REQUIRE(call.run("list.stable_sort").ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({1, 2, 0}));
  call.inputs = {{"a", Data::record(values::string("中文"))},
                 {"b", Data::record(values::string(""))}};
  REQUIRE(call.run("value.concat", {{"connector", Value::text(" | ")}}).ok);
  CHECK(call.out("value").asRecord()->data["value"] == "中文 | ");
  call.inputs["b"] = Data::record(values::string(std::string(values::kMaxStringBytes, 'x')));
  CHECK_FALSE(call.run("value.concat").ok);
}

TEST_CASE("std-value: scalar predicate selects lazily and named outputs retain versioned records") {
  auto graph =
      makeGraph({N{"a", "value.number", {{"value", 18}, {"unit", "px²"}}},
                 N{"b", "value.number", {{"value", 10}, {"unit", "px²"}}},
                 N{"compare", "value.compare", {{"operation", "greater"}}},
                 N{"pass", "value.string", {{"value", "合格"}}}, N{"zero", "value.number"},
                 N{"fail", "value.arithmetic", {{"operation", "divide"}}},
                 N{"select", "flow.select", {{"path", "value"}}}},
                {E{"a.value", "compare.a"}, E{"b.value", "compare.b"},
                 E{"compare.value", "select.cond"}, E{"pass.value", "select.a"},
                 E{"a.value", "fail.a"}, E{"zero.value", "fail.b"}, E{"fail.value", "select.b"}});
  graph["outputs"] = {{"result", {{"node", "select"}, {"port", "out"}}}};
  Session session(graph);
  const auto& log = session.wait();
  CHECK(log.runStatus() == "ok");
  CHECK(log.finalState("select") == "done");
  CHECK(log.finalState("fail") == "skipped");
  CHECK(log.nodeEvent("fail", "skipped")["stats"]["reason"] == "not_demanded");
  Data result;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "select", "out", result));
  REQUIRE(result.asRecord());
  CHECK(result.asRecord()->type == values::kValueType);
  CHECK(result.asRecord()->data["value"] == "合格");
  const auto named = exec::ResultStore::instance().namedOutputs(session.runId());
  REQUIRE(named.size() == 1);
  CHECK(named.front().name == "result");
}

TEST_CASE(
    "std-value: float64 contract rejects unsafe JSON integers and sorts mixed numbers stably") {
  constexpr std::int64_t boundary = values::kMaxSafeInteger;
  std::string why;
  CHECK(values::validate(values::makeValue("number", boundary), why));
  CHECK(values::validate(values::makeValue("number", -boundary), why));
  CHECK(values::validate(values::makeValue("number", static_cast<std::uint64_t>(boundary)), why));
  CHECK_FALSE(values::validate(values::makeValue("number", boundary + 1), why));
  CHECK_FALSE(values::validate(values::makeValue("number", -boundary - 1), why));
  CHECK_FALSE(values::validate(
      values::makeValue("number", std::numeric_limits<std::uint64_t>::max()), why));
  CHECK(values::validate(values::number(1e18), why));

  OpCall call;
  call.inputs = {{"a", Data::record(values::makeValue("number", boundary))},
                 {"b", Data::record(values::makeValue("number", boundary + 1))}};
  const std::unordered_map<std::string, Value> exact{{"absoluteTolerance", Value::number(0)},
                                                     {"relativeTolerance", Value::number(0)}};
  CHECK_FALSE(call.run("value.compare", exact).ok);
  call.inputs["b"] = value(static_cast<double>(boundary));
  REQUIRE(call.run("value.compare", exact).ok);
  CHECK(call.out("value").asRecord()->data["value"] == true);
  CHECK_FALSE(
      call.run("list.constant", {{"items", Value::text("[9007199254740992,9007199254740993]")}})
          .ok);
  CHECK_FALSE(call.run("list.constant", {{"items", Value::text("[18446744073709551615]")}}).ok);

  const Json mixed = Json::array(
      {1.0, 1, std::uint64_t{1}, 0.5, -1, 2, 2.0, static_cast<double>(boundary), boundary});
  call.inputs = {{"list", list(mixed)}};
  REQUIRE(call.run("list.stable_sort").ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({4, 3, 0, 1, 2, 5, 6, 7, 8}));
  REQUIRE(call.run("list.stable_sort", {{"direction", Value::text("descending")}}).ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({7, 8, 5, 6, 0, 1, 2, 3, 4}));

  const Json records = Json::array(
      {Json{{"type", "test.Part.v1"}, {"data", {{"id", "18446744073709551615"}, {"score", 1.0}}}},
       Json{{"type", "test.Part.v1"}, {"data", {{"id", "b"}, {"score", std::uint64_t{1}}}}},
       Json{{"type", "test.Part.v1"}, {"data", {{"id", "c"}, {"score", 1}}}}});
  call.inputs = {{"list", list(records, "record")}};
  REQUIRE(call.run("list.stable_sort", {{"pointer", Value::text("/score")}}).ok);
  CHECK(call.out("indices").asRecord()->data["items"] == Json::array({0, 1, 2}));
  auto unsafe = records;
  unsafe[1]["data"]["score"] = boundary + 1;
  call.inputs = {{"list", list(unsafe, "record")}};
  CHECK_FALSE(call.run("list.stable_sort", {{"pointer", Value::text("/score")}}).ok);
  call.inputs = {{"record", Data::record(Record{"test.Part.v1", {{"score", boundary + 1}}})}};
  CHECK_FALSE(call.run("value.field", {{"pointer", Value::text("/score")}}).ok);
}

} // namespace lyflow::test
