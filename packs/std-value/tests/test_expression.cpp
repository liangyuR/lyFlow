#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow/value.h"

namespace lyflow::test {
namespace {

Data expressionNumber(double number, const std::string& unit = {}) {
  return Data::record(values::number(number, unit));
}

Status expression(OpCall& call, const std::string& source) {
  return call.run("value.expression", {{"expression", Value::text(source)}});
}

void expressionNumberIs(OpCall& call, const std::string& source, double expected,
                        const std::string& unit = {}) {
  const auto status = expression(call, source);
  REQUIRE_MESSAGE(status.ok, source, ": ", status.message);
  const auto* record = call.out("value").asRecord();
  REQUIRE(record != nullptr);
  std::string why;
  REQUIRE_MESSAGE(values::validate(*record, why), why);
  CHECK(record->data["kind"] == "number");
  CHECK(record->data["value"].get<double>() == doctest::Approx(expected).epsilon(1e-12));
  CHECK(record->data["unit"] == unit);
}

void expressionBooleanIs(OpCall& call, const std::string& source, bool expected) {
  const auto status = expression(call, source);
  REQUIRE_MESSAGE(status.ok, source, ": ", status.message);
  const auto* record = call.out("value").asRecord();
  REQUIRE(record != nullptr);
  std::string why;
  REQUIRE_MESSAGE(values::validate(*record, why), why);
  CHECK(record->data["kind"] == "boolean");
  CHECK(record->data["value"] == expected);
  CHECK(record->data["unit"] == "");
}

void expressionFails(OpCall& call, const std::string& source,
                     const char* expectedCode = "bad_input") {
  const auto status = expression(call, source);
  CAPTURE(source);
  CHECK_FALSE(status.ok);
  CHECK(status.code == expectedCode);
  CHECK_FALSE(status.message.empty());
  CHECK(call.outputs.empty());
  if (std::string(expectedCode) == "bad_param")
    CHECK(status.paramPath == "expression");
}

std::string balancedExpression(std::size_t leaves) {
  if (leaves == 1)
    return "1";
  const auto left = leaves / 2;
  return "(" + balancedExpression(left) + "+" + balancedExpression(leaves - left) + ")";
}

class ExpressionCancellation final : public ExecContext {
public:
  explicit ExpressionCancellation(std::size_t after) : after_(after) {}
  bool cancelled() const override { return ++checks >= after_; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }
  mutable std::size_t checks = 0;

private:
  std::size_t after_;
  std::filesystem::path base_;
};

} // namespace

TEST_CASE(
    "std-value expression: manifest and static validation expose one bounded scalar contract") {
  const auto* descriptor = ensureRegistry().find("value.expression");
  REQUIRE(descriptor != nullptr);
  CHECK(descriptor->version == "1.0.0");
  REQUIRE(descriptor->inputs.size() == 8);
  for (std::size_t index = 0; index < descriptor->inputs.size(); ++index) {
    const auto& port = descriptor->inputs[index];
    CHECK(port.name == std::string(1, static_cast<char>('a' + index)));
    CHECK(port.type == "Record");
    CHECK_FALSE(port.required);
    CHECK_FALSE(port.lazy);
    CHECK(port.contract["recordType"] == values::kValueType);
  }
  REQUIRE(descriptor->outputs.size() == 1);
  CHECK(descriptor->outputs.front().name == "value");
  CHECK(descriptor->outputs.front().contract["recordType"] == values::kValueType);
  REQUIRE(descriptor->validate != nullptr);
  OpCall call;
  for (const auto* source : {"(a+b)/2", "true ? 2 : h", "sqrt(a)", "false && (a > 0)"}) {
    CAPTURE(source);
    CHECK(call.validate("value.expression", {{"expression", Value::text(source)}}, {}).empty());
  }
  for (const auto* source : {"1 + )", "false ? abs() : 1", "true || unknown", "a = 2"}) {
    CAPTURE(source);
    const auto issues =
        call.validate("value.expression", {{"expression", Value::text(source)}}, {"a"});
    REQUIRE_FALSE(issues.empty());
    CHECK((issues.front().severity == Severity::Error));
    CHECK(issues.front().status.code == "bad_param");
    CHECK(issues.front().status.paramPath == "expression");
  }
}

TEST_CASE(
    "std-value expression: precedence real literals and whitelisted functions have explicit math") {
  OpCall call;
  call.inputs = {{"a", expressionNumber(4)}, {"b", expressionNumber(8)}};
  REQUIRE(call.run("value.expression").ok);
  CHECK(call.out("value").asRecord()->data["value"] == 6);
  const std::vector<std::pair<std::string, double>> cases{{"1 + 2 * 3 ^ 2", 19},
                                                          {"(1+2)*3", 9},
                                                          {"2^3^2", 512},
                                                          {"-2^2", -4},
                                                          {"(-2)^2", 4},
                                                          {"2^-2", 0.25},
                                                          {"-2^-2", -0.25},
                                                          {"2 * -3", -6},
                                                          {"\t .5 + 1. + 2E-1\r\n", 1.7},
                                                          {"1e3 / 2e+2", 5},
                                                          {"abs(-3)", 3},
                                                          {"min(-3,2)", -3},
                                                          {"max(-3,2)", 2},
                                                          {"clamp(4,1,3)", 3},
                                                          {"clamp(-1,1,3)", 1},
                                                          {"sqrt(2.25)", 1.5},
                                                          {"pow(2,-3)", 0.125},
                                                          {"floor(-2.1)", -3},
                                                          {"ceil(-2.1)", -2},
                                                          {"round(2.5)", 3},
                                                          {"round(-2.5)", -3},
                                                          {"false ? 1 : true ? 7 : 9", 7},
                                                          {"true ? false ? 1 : 2 : 3", 2}};
  for (const auto& item : cases)
    expressionNumberIs(call, item.first, item.second);
  call.inputs.clear();
  for (char name = 'a'; name <= 'h'; ++name)
    call.inputs.emplace(std::string(1, name), expressionNumber(name - 'a' + 1));
  expressionNumberIs(call, "a+b+c+d+e+f+g+h", 36);
  expressionNumberIs(call, "1e308", 1e308);
  REQUIRE(expression(call, "1e-300").ok);
  CHECK(call.out("value").asRecord()->data["value"].get<double>() == 1e-300);
  REQUIRE(expression(call, "1e-200 * 1e-200").ok);
  CHECK(call.out("value").asRecord()->data["value"] == 0.0);
}

TEST_CASE("std-value expression: units match scalar arithmetic and never infer conversions") {
  OpCall call;
  call.inputs = {{"a", expressionNumber(12.25, "mm")},
                 {"b", expressionNumber(3, "mm")},
                 {"c", expressionNumber(2)}};
  for (const auto& item : std::vector<std::pair<const char*, double>>{{"a+b", 15.25},
                                                                      {"a-b", 9.25},
                                                                      {"a*c", 24.5},
                                                                      {"c*a", 24.5},
                                                                      {"a/c", 6.125},
                                                                      {"abs(-a)", 12.25},
                                                                      {"floor(a)", 12},
                                                                      {"ceil(a)", 13},
                                                                      {"round(a)", 12},
                                                                      {"min(a,b)", 3},
                                                                      {"max(a,b)", 12.25},
                                                                      {"clamp(a,b,b)", 3}})
    expressionNumberIs(call, item.first, item.second, "mm");
  expressionNumberIs(call, "a/b", 12.25 / 3);
  expressionBooleanIs(call, "a > b", true);
  expressionBooleanIs(call, "abs(a-b) <= a", true);
  for (const auto* source : {"a+c", "a-c", "a*b", "c/a", "a^c", "pow(a,c)", "sqrt(a)", "a==c",
                             "min(a,c)", "max(a,c)", "clamp(a,b,c)", "clamp(b,a,b)"})
    expressionFails(call, source);
  call.inputs["b"] = expressionNumber(3, "m");
  for (const auto* source : {"a+b", "a-b", "a/b", "a<b", "a!=b", "min(a,b)"})
    expressionFails(call, source);
}

TEST_CASE(
    "std-value expression: finite number and boolean typing rejects malformed native records") {
  OpCall call;
  const auto original = values::number(7.5, "m");
  call.inputs = {{"a", Data::record(original)}, {"b", Data::record(values::boolean(true))}};
  expressionNumberIs(call, "b ? a : 0", 7.5, "m");
  expressionBooleanIs(call, "b == true", true);
  for (const auto* source : {"a+b", "!a", "a && b", "b ? b+1 : 0", "a == b", "b < true",
                             "a ? 1 : 0", "abs(b)", "1 < 2 < 3"})
    expressionFails(call, source);
  CHECK(call.inputs.at("a").asRecord()->data == original.data);
  const auto missing = expression(call, "h");
  CHECK_FALSE(missing.ok);
  CHECK(missing.code == "bad_input");
  CHECK(missing.portName == "h");
  CHECK(call.outputs.empty());
  std::vector<Data> invalid{Data{},
                            Data::record(values::string("12")),
                            Data::record(values::makeList("number", Json::array({1}))),
                            Data::record(Record{"std.Value.v2", values::number(1).data}),
                            Data::record(values::makeValue("number", nullptr)),
                            Data::record(values::makeValue("boolean", true, "m")),
                            Data::record(values::number(std::numeric_limits<double>::infinity())),
                            Data::record(values::number(std::numeric_limits<double>::quiet_NaN())),
                            Data::record(values::makeValue("number", values::kMaxSafeInteger + 1))};
  for (const auto& value : invalid) {
    call.inputs["a"] = value;
    expressionFails(call, "a");
  }
  call.inputs = {{"a", expressionNumber(std::numeric_limits<double>::max())},
                 {"b", expressionNumber(-0.0)}};
  for (const auto* source :
       {"a+a", "a*2", "a/b", "sqrt(-1)", "pow(-2,.5)", "0^0", "0^-1", "1/0", "clamp(1,3,2)"})
    expressionFails(call, source);
}

TEST_CASE(
    "std-value expression: short circuit skips only runtime evaluation and parses every branch") {
  OpCall call;
  expressionBooleanIs(call, "false && (a > 0)", false);
  expressionBooleanIs(call, "true || (1/0 > 0)", true);
  expressionNumberIs(call, "true ? 2 : a", 2);
  expressionNumberIs(call, "false ? sqrt(-1) : 3", 3);
  expressionNumberIs(call, "false ? 1/0 : true ? 4 : a", 4);
  expressionFails(call, "true && (a > 0)");
  expressionFails(call, "false || (1/0 > 0)");
  expressionFails(call, "false ? 2 : a");
  for (const auto* source : {"true ? 1 : abs()", "true || unknown", "false && system(1)",
                             "true ? 1 : (2 + )", "false ? pow(2) : 1", "true ? 1 : 2 3"})
    expressionFails(call, source, "bad_param");
  expressionBooleanIs(call, "1 != 2 && 2 <= 2 && 3 >= 2 && !(2 > 3) && 1 < 2", true);
  expressionBooleanIs(call, "0.1 + 0.2 == 0.3", false);
  expressionBooleanIs(call, "true != false", true);
  expressionFails(call, "1 && true");
}

TEST_CASE(
    "std-value expression: invalid syntax injection and resource budgets fail without output") {
  OpCall call;
  const std::vector<std::string> invalid{
      "",          " ",        "1 +",        "(1+2",
      "1+2)",      "1 2",      "1..2",       ".",
      "1e",        "1e+",      "1e309",      "1e-999",
      "0x10",      "NaN",      "Inf",        "a=2",
      "a+=2",      "a++",      "a[0]",       "a.value",
      "a;2",       "\"text\"", "'text'",     "__import__('os')",
      "sqrt(1,2)", "max(1)",   "clamp(1,2)", "Abs(1)",
      "宽度+1",    "true ? 1", "1,2",        std::string("1\0+2", 4)};
  for (const auto& source : invalid)
    expressionFails(call, source, "bad_param");
  const auto position = expression(call, "1 + )");
  CHECK(position.message.find('5') != std::string::npos);
  expressionNumberIs(call, std::string(4095, ' ') + "1", 1);
  expressionFails(call, std::string(4096, ' ') + "1", "bad_param");
  expressionNumberIs(call, std::string(64, '(') + "1" + std::string(64, ')'), 1);
  expressionFails(call, std::string(65, '(') + "1" + std::string(65, ')'), "bad_param");
  expressionNumberIs(call, std::string(63, '+') + "1", 1);
  expressionFails(call, std::string(64, '+') + "1", "bad_param");
  const auto balanced = balancedExpression(512);
  expressionNumberIs(call, balanced, 512);
  expressionNumberIs(call, "+" + balanced, 512);
  expressionFails(call, "++" + balanced, "bad_param");
  expressionFails(call, balancedExpression(513), "bad_param");
  std::string left = "1";
  std::string powers = "1";
  for (int i = 0; i < 80; ++i) {
    left += "+1";
    powers += "^1";
  }
  expressionFails(call, left, "bad_param");
  expressionFails(call, powers, "bad_param");
}

TEST_CASE(
    "std-value expression: cancellation polls during bounded work and never publishes partial data") {
  const auto* descriptor = ensureRegistry().find("value.expression");
  REQUIRE(descriptor != nullptr);
  ParamMap params;
  for (const auto& param : descriptor->params)
    params[param.name] = param.def;
  params["expression"] = Value::text(balancedExpression(256));
  const std::filesystem::path base;
  const ParamView view(params, base);
  const std::unordered_map<std::string, Data> values;
  const Inputs inputs(values);
  for (const std::size_t after : {std::size_t{1}, std::size_t{5}}) {
    ExpressionCancellation context(after);
    std::unordered_map<std::string, Data> collected;
    Outputs outputs(collected);
    const auto status = descriptor->compute(inputs, view, outputs, context);
    CHECK((status.ok || status.code == "cancelled"));
    CHECK(context.checks >= after);
    CHECK(collected.empty());
  }
}

TEST_CASE(
    "std-value expression: scheduler preserves eager input dependencies and explicit lazy selection") {
  auto graph = makeGraph({N{"bad", "value.expression", {{"expression", "1/0"}}},
                          N{"result", "value.expression", {{"expression", "true || a"}}}},
                         {E{"bad.value", "result.a"}});
  graph["outputs"] = {{"result", {{"node", "result"}, {"port", "value"}}}};
  {
    Session session(graph);
    const auto& log = session.wait();
    CHECK(log.runStatus() == "error");
    REQUIRE(log.ofKind("run_finished").size() == 1);
    CHECK(log.ofKind("run_finished").front()["summary"]["status"] == "failed");
    CHECK(log.finalState("bad") == "error");
    CHECK(log.finalState("result") != "done");
  }
  graph = makeGraph({N{"a", "value.number", {{"value", 12}, {"unit", "mm"}}},
                     N{"b", "value.number", {{"value", 14}, {"unit", "mm"}}},
                     N{"limit", "value.number", {{"value", 15}, {"unit", "mm"}}},
                     N{"mean", "value.expression", {{"expression", "(a+b)/2"}}},
                     N{"predicate", "value.expression", {{"expression", "a <= b"}}},
                     N{"bad", "value.expression", {{"expression", "1/0"}}},
                     N{"select", "flow.select", {{"path", "value"}}}},
                    {E{"a.value", "mean.a"}, E{"b.value", "mean.b"}, E{"mean.value", "predicate.a"},
                     E{"limit.value", "predicate.b"}, E{"predicate.value", "select.cond"},
                     E{"mean.value", "select.a"}, E{"bad.value", "select.b"}});
  graph["outputs"] = {{"result", {{"node", "select"}, {"port", "out"}}}};
  Session session(graph);
  const auto& log = session.wait();
  REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
  CHECK(log.finalState("bad") == "skipped");
  CHECK(log.nodeEvent("bad", "skipped")["stats"]["reason"] == "not_demanded");
  Data result;
  REQUIRE(exec::ResultStore::instance().get(session.runId(), "select", "out", result));
  REQUIRE(result.asRecord() != nullptr);
  CHECK(result.asRecord()->type == values::kValueType);
  CHECK(result.asRecord()->data["value"] == 13);
  CHECK(result.asRecord()->data["unit"] == "mm");
}

} // namespace lyflow::test
