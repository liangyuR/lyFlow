#include "lyflow/kernel/constraints.h"

#include <cmath>
#include <limits>
#include <type_traits>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
using namespace lyflow::kernel::constraints;

Json point(const std::string& id = "p", double weight = 1) {
  return {{"id", id},
          {"type", "point"},
          {"source", {0, 1, 2}},
          {"target", {3, 4, 5}},
          {"weight", weight}};
}

Json record(Json items) {
  return {{"sourceFrame", "source"},
          {"targetFrame", "target"},
          {"unit", "m"},
          {"items", std::move(items)}};
}

Json record(std::initializer_list<Json> items) {
  Json array = Json::array();
  for (const auto& item : items)
    array.push_back(item);
  return record(std::move(array));
}

bool parse(const Json& data, std::vector<Constraint>& result, Error& error,
           const std::string& mode = "mixed") {
  return parse3D(data, "source", "target", mode, result, error);
}
} // namespace

TEST_CASE("Constraints kernel preserves construction data and normalizes parsed arrays") {
  static_assert(std::is_same_v<decltype(Constraint::source), std::array<double, 3>>);
  Json items = {point("p", 2), point("line", 4), point("plane", 1), point("axis", 3)};
  for (std::size_t i = 1; i < items.size(); ++i) {
    items[i]["type"] = i == 1 ? "line" : i == 2 ? "plane" : "axis";
    items[i]["direction"] = {0, 3, 4};
  }
  items[2]["offset"] = -0.25;
  items[3]["datum"] = 2;
  Json out;
  Error error;
  REQUIRE(construct3D(items.dump(), "source", "target", "mixed", out, error));
  CHECK(out == record(items));
  std::vector<Constraint> parsed;
  REQUIRE(parse(out, parsed, error));
  REQUIRE(parsed.size() == 4);
  CHECK(parsed[0].weight == 0.5);
  CHECK(parsed[1].weight == 1);
  CHECK(parsed[1].direction[1] == doctest::Approx(0.6));
  CHECK(parsed[1].direction[2] == doctest::Approx(0.8));
  CHECK(parsed[2].offset == -0.25);
  CHECK(parsed[3].datum == 2);
  CHECK(error.code.empty());
  const auto stable = parsed;
  Json broken = out;
  broken["items"][1]["id"] = "p";
  CHECK_FALSE(parse(broken, parsed, error));
  REQUIRE(parsed.size() == stable.size());
  CHECK(parsed[1].id == stable[1].id);
  CHECK(parsed[1].direction == stable[1].direction);
}

TEST_CASE("Constraints kernel rejects invalid schema and finite numeric boundaries atomically") {
  const Json good = record({point()});
  std::vector<Json> invalid;
  auto add = [&](const char* key, Json value) {
    Json copy = good;
    copy["items"][0][key] = std::move(value);
    invalid.push_back(std::move(copy));
  };
  add("id", "");
  add("id", std::string(1025, 'x'));
  add("type", "unknown");
  add("source", {0, 1});
  add("target", {0, 1, 1e12 + 1});
  add("weight", true);
  add("weight", 0);
  add("weight", -1);
  add("weight", std::numeric_limits<double>::infinity());
  add("weight", std::uint64_t(9007199254740993ULL));
  add("unused", 1);
  add("direction", {1, 0, 0});
  add("offset", 0);
  add("datum", 0);
  for (const char* type : {"line", "plane", "axis"}) {
    Json copy = good;
    copy["items"][0]["type"] = type;
    copy["items"][0]["direction"] = {0, 0, 0};
    invalid.push_back(copy);
    copy["items"][0]["direction"] = {1, 0, 0};
    copy["items"][0][type == std::string("line") ? "offset" : "datum"] = 0.5;
    invalid.push_back(copy);
  }
  for (const char* key : {"sourceFrame", "targetFrame", "unit", "unknown"}) {
    Json copy = good;
    copy[key] = "mismatch";
    invalid.push_back(copy);
  }
  Json missing = good;
  missing.erase("unit");
  invalid.push_back(missing);
  Json malformed = good;
  malformed["items"][0]["id"] = std::string(1, static_cast<char>(0xff));
  invalid.push_back(malformed);
  for (const auto& data : invalid) {
    std::vector<Constraint> result(1);
    result[0].id = "sentinel";
    Error error;
    CHECK_FALSE(parse(data, result, error));
    CHECK(result.size() == 1);
    CHECK(result[0].id == "sentinel");
    CHECK(error.code == "bad_input");
    CHECK_FALSE(error.message.empty());
  }
  Error error;
  std::vector<Constraint> parsed;
  CHECK_FALSE(parse3D(good, "", "target", "mixed", parsed, error));
  CHECK(error.field == "sourceFrame");
  CHECK(error.code == "bad_param");
  CHECK_FALSE(parse(good, parsed, error, "unknown"));
  CHECK(error.field == "mode");
}

TEST_CASE("Constraints kernel handles weight ratio vector magnitude and bounded inputs") {
  std::vector<Constraint> parsed;
  Error error;
  REQUIRE(parse(record({point("min", 1), point("max", 1e12)}), parsed, error));
  CHECK(parsed[0].weight == 1e-12);
  CHECK_FALSE(parse(record({point("min", 1), point("max", 1e12 + 1)}), parsed, error));
  Json line = point();
  line["type"] = "line";
  for (const double value : {1e308, std::numeric_limits<double>::denorm_min()}) {
    line["direction"] = {value, 0, 0};
    REQUIRE(parse(record({line}), parsed, error));
    CHECK(parsed[0].direction[0] == doctest::Approx(1).epsilon(2e-16));
  }
  line["direction"] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0};
  CHECK_FALSE(parse(record({line}), parsed, error));
  Json many = Json::array();
  for (std::size_t i = 0; i < kMaxConstraints; ++i)
    many.push_back(point(std::to_string(i)));
  REQUIRE(parse(record(many), parsed, error));
  CHECK(parsed.size() == kMaxConstraints);
  many.push_back(point("extra"));
  CHECK_FALSE(parse(record(many), parsed, error));
  Json large = Json::array();
  for (unsigned i = 0; i < 2048; ++i)
    large.push_back(point(std::to_string(i) + std::string(1000, 'x')));
  CHECK_FALSE(parse(record(large), parsed, error));
  CHECK(error.message.find("1 MiB") != std::string::npos);
  const Json sentinel = {{"unchanged", true}};
  Json output = sentinel;
  const std::string text = Json::array({point()}).dump();
  REQUIRE(construct3D(text + std::string(kMaxItemsTextBytes - text.size(), ' '), "source", "target",
                      "mixed", output, error));
  for (const std::string bad : {std::string("["), std::string("[]"), std::string("{}"),
                                std::string("[{}]"), std::string(kMaxItemsTextBytes + 1, ' ')}) {
    output = sentinel;
    CHECK_FALSE(construct3D(bad, "source", "target", "mixed", output, error));
    CHECK(output == sentinel);
    CHECK(error.code == "bad_param");
    CHECK(error.field == "items");
  }
  output = sentinel;
  CHECK_FALSE(construct3D(text, "source", "target", "unknown", output, error));
  CHECK(error.code == "bad_param");
  CHECK(error.field == "items");
  CHECK(output == sentinel);
}

TEST_CASE("Constraints kernel enforces weighted 321 datum counts and orthogonal parallel groups") {
  Json items = Json::array();
  for (int i = 0; i < 6; ++i) {
    Json item = point(std::to_string(i));
    const int datum = i < 3 ? 0 : i < 5 ? 1 : 2;
    item["type"] = i % 2 ? "plane" : "axis";
    item["datum"] = datum;
    item["direction"] = datum == 0   ? Json{0, 0, i == 1 ? -2 : 2}
                        : datum == 1 ? Json{0, 3, 0}
                                     : Json{4, 0, 0};
    items.push_back(item);
  }
  std::vector<Constraint> parsed;
  Error error;
  REQUIRE(parse(record(items), parsed, error, "weighted_321"));
  for (int kind = 0; kind < 5; ++kind) {
    Json bad = items;
    if (kind == 0)
      bad.erase(bad.begin());
    else if (kind == 1)
      bad[0].erase("datum");
    else if (kind == 2)
      bad[1]["direction"] = {0, 1, 0};
    else if (kind == 3)
      bad[5]["direction"] = {0, 0, 1};
    else {
      bad[0]["type"] = "line";
      bad[0].erase("datum");
    }
    CAPTURE(kind);
    CHECK_FALSE(parse(record(bad), parsed, error, "weighted_321"));
  }
}
