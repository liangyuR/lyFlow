#include "lyflow/kernel/constraints.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "lyflow/value.h"

namespace lyflow::kernel::constraints {
namespace {
using Json = nlohmann::json;

bool fail(Error& error, const std::string& message, const char* field = "constraints",
          const char* code = "bad_input") {
  error = {code, field, message};
  return false;
}

using values::safeNumber;

bool keys(const Json& item, std::initializer_list<const char*> allowed) {
  if (!item.is_object())
    return false;
  std::set<std::string> names;
  for (const char* name : allowed)
    names.insert(name);
  for (auto it = item.begin(); it != item.end(); ++it)
    if (!names.count(it.key()))
      return false;
  return true;
}

bool textField(const Json& object, const char* key) {
  return object.contains(key) && object[key].is_string() &&
         !object[key].get_ref<const std::string&>().empty();
}

bool numericVector(const Json& value) {
  return value.is_array() && value.size() == 3 &&
         std::all_of(value.begin(), value.end(), [](const Json& n) { return safeNumber(n); });
}

double dot(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double stableNorm(const std::array<double, 3>& vector) {
  double scale = std::max({std::abs(vector[0]), std::abs(vector[1]), std::abs(vector[2])});
  if (scale == 0)
    return 0;
  double inverse = 1 / scale;
  if (inverse > std::numeric_limits<double>::max()) {
    inverse = std::numeric_limits<double>::max();
    scale = 1 / inverse;
  }
  const double x = vector[0] * inverse, y = vector[1] * inverse, z = vector[2] * inverse;
  return scale * std::sqrt(x * x + y * y + z * z);
}

bool frameValid(const std::string& frame) { return !frame.empty() && frame.size() <= 1024; }
} // namespace

bool parse3D(const Json& data, const std::string& sourceFrame, const std::string& targetFrame,
             const std::string& mode, std::vector<Constraint>& out, Error& error) {
  if (!frameValid(sourceFrame) || !frameValid(targetFrame))
    return fail(error, "源/目标帧必须显式有效", "sourceFrame", "bad_param");
  std::string why;
  if (!values::boundedJson(data, why))
    return fail(error, "需要有界cloud.Constraints3D.v1约束记录：" + why);
  if (!keys(data, {"sourceFrame", "targetFrame", "unit", "items"}) || data.size() != 4 ||
      data.value("sourceFrame", Json{}) != sourceFrame ||
      data.value("targetFrame", Json{}) != targetFrame || data.value("unit", Json{}) != "m" ||
      !data.contains("items") || !data["items"].is_array() || data["items"].empty() ||
      data["items"].size() > kMaxConstraints)
    return fail(error, "约束必须有匹配的sourceFrame/targetFrame、米制unit与1..4096项items");
  std::vector<Constraint> result;
  result.reserve(data["items"].size());
  std::set<std::string> ids;
  double maximumWeight = 0;
  for (const auto& item : data["items"]) {
    if (!keys(item, {"id", "type", "source", "target", "direction", "weight", "offset", "datum"}) ||
        !textField(item, "id") || !textField(item, "type") || !item.contains("source") ||
        !numericVector(item["source"]) || !item.contains("target") ||
        !numericVector(item["target"]) || !item.contains("weight") || !safeNumber(item["weight"]))
      return fail(error, "约束需要唯一id、type、有限三维source/target及正weight");
    Constraint c;
    c.id = item["id"];
    c.type = item["type"];
    c.source = item["source"].get<std::array<double, 3>>();
    c.target = item["target"].get<std::array<double, 3>>();
    c.weight = item["weight"].get<double>();
    if (!ids.insert(c.id).second || c.id.size() > 1024 || !std::isfinite(c.weight) ||
        c.weight <= 0 ||
        std::any_of(c.source.begin(), c.source.end(),
                    [](double v) { return std::abs(v) > 1e12; }) ||
        std::any_of(c.target.begin(), c.target.end(), [](double v) { return std::abs(v) > 1e12; }))
      return fail(error, "约束id重复、weight非正或坐标超过1e12米预算");
    if (c.type != "point" && c.type != "line" && c.type != "plane" && c.type != "axis")
      return fail(error, "约束type仅支持point/line/plane/axis");
    if (c.type == "point") {
      if (item.contains("direction") || item.contains("offset") || item.contains("datum"))
        return fail(error, "point不消费direction/offset/datum字段");
    } else {
      if (!item.contains("direction") || !numericVector(item["direction"]))
        return fail(error, "线方向或平面/轴向法向必须是非零三维向量");
      c.direction = item["direction"].get<std::array<double, 3>>();
      const double magnitude = stableNorm(c.direction);
      if (!(magnitude > 0) || !std::isfinite(magnitude))
        return fail(error, "direction必须有限且非零");
      for (auto& component : c.direction)
        component /= magnitude;
      if (c.type == "line") {
        if (item.contains("offset") || item.contains("datum"))
          return fail(error, "line不消费offset/datum字段");
      } else {
        if (item.contains("offset")) {
          if (!safeNumber(item["offset"]) || std::abs(item["offset"].get<double>()) > 1e12)
            return fail(error, "offset必须为有限米制有符号偏移");
          c.offset = item["offset"].get<double>();
        }
        if (item.contains("datum")) {
          if (!item["datum"].is_number_integer() || item["datum"] < 0 || item["datum"] > 2)
            return fail(error, "datum必须为整数0/1/2");
          c.datum = item["datum"].get<int>();
        }
      }
    }
    maximumWeight = std::max(maximumWeight, c.weight);
    result.push_back(std::move(c));
  }
  for (auto& c : result) {
    c.weight /= maximumWeight;
    if (c.weight < 1e-12)
      return fail(error, "最大/最小权重之比不得超过1e12");
  }
  if (mode == "weighted_321") {
    std::array<int, 3> count{};
    std::array<std::array<double, 3>, 3> axes{};
    for (const auto& c : result) {
      if ((c.type != "axis" && c.type != "plane") || c.datum < 0)
        return fail(error, "weighted_321仅接受带datum=0/1/2的axis/plane约束");
      if (count[c.datum]++ == 0)
        axes[c.datum] = c.direction;
      else if (std::abs(dot(axes[c.datum], c.direction)) < 1 - 1e-8)
        return fail(error, "同datum的轴向约束必须平行");
    }
    if (count[0] < 3 || count[1] < 2 || count[2] < 1 || std::abs(dot(axes[0], axes[1])) > 1e-8 ||
        std::abs(dot(axes[0], axes[2])) > 1e-8 || std::abs(dot(axes[1], axes[2])) > 1e-8)
      return fail(error, "weighted_321需要正交的3/2/1及以上有效datum约束");
  } else if (mode != "mixed") {
    return fail(error, "未知约束拟合模式", "mode", "bad_param");
  }
  out = std::move(result);
  error = {};
  return true;
}

bool construct3D(const std::string& itemsText, const std::string& sourceFrame,
                 const std::string& targetFrame, const std::string& mode, Json& out, Error& error) {
  if (!frameValid(sourceFrame) || !frameValid(targetFrame))
    return fail(error, "源/目标帧必须显式有效", "sourceFrame", "bad_param");
  if (itemsText.empty() || itemsText.size() > kMaxItemsTextBytes)
    return fail(error, "items必须为1..65536字节的约束JSON数组", "items", "bad_param");
  const auto items = Json::parse(itemsText, nullptr, false);
  if (items.is_discarded())
    return fail(error, "items不是合法JSON数组", "items", "bad_param");
  Json candidate{
      {"sourceFrame", sourceFrame}, {"targetFrame", targetFrame}, {"unit", "m"}, {"items", items}};
  std::vector<Constraint> parsed;
  if (!parse3D(candidate, sourceFrame, targetFrame, mode, parsed, error)) {
    error.code = "bad_param";
    error.field = "items";
    return false;
  }
  out = std::move(candidate);
  error = {};
  return true;
}
} // namespace lyflow::kernel::constraints
