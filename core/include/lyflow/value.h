#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/data.h"

namespace lyflow::values {

inline constexpr const char* kValueType = "std.Value.v1";
inline constexpr const char* kListType = "std.List.v1";
inline constexpr std::size_t kMaxItems = 4096;
inline constexpr std::size_t kMaxBytes = 1024 * 1024;
inline constexpr std::size_t kMaxStringBytes = 65536;
inline constexpr std::size_t kMaxDepth = 16;
inline constexpr std::int64_t kMaxSafeInteger = 9007199254740992LL;

inline bool safeNumber(const nlohmann::json& value) {
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(kMaxSafeInteger);
  if (value.is_number_integer()) {
    const auto integer = value.get<std::int64_t>();
    return integer >= -kMaxSafeInteger && integer <= kMaxSafeInteger;
  }
  return value.is_number_float() && std::isfinite(value.get<double>());
}

inline bool boundedJson(const nlohmann::json& json, std::string& why, bool allowNull = false) {
  std::vector<std::pair<const nlohmann::json*, std::size_t>> pending{{&json, 0}};
  std::size_t count = 0;
  std::size_t bytes = 0;
  while (!pending.empty()) {
    const auto item = pending.back();
    pending.pop_back();
    const auto& value = *item.first;
    if (item.second > kMaxDepth || ++count > kMaxBytes) {
      why = "JSON 嵌套深度或元素数超过限制";
      return false;
    }
    if (value.is_discarded() || value.is_binary() || (!allowNull && value.is_null())) {
      why = "值不允许 null、binary 或 discarded";
      return false;
    }
    if (value.is_number() && !safeNumber(value)) {
      why = "数值必须有限；JSON 整数必须位于 [-2^53,2^53]，更大的标识请使用字符串";
      return false;
    }
    if (value.is_string() && value.get_ref<const std::string&>().size() > kMaxStringBytes) {
      why = "字符串超过 64 KiB";
      return false;
    }
    bytes += value.is_string() ? value.get_ref<const std::string&>().size() + 2 : 2;
    if (value.is_structured()) {
      if (value.size() > kMaxItems) {
        why = "集合或对象超过 4096 项";
        return false;
      }
      for (auto child = value.begin(); child != value.end(); ++child) {
        if (value.is_object()) {
          if (child.key().size() > kMaxStringBytes) {
            why = "字段名超过 64 KiB";
            return false;
          }
          bytes += child.key().size() + 3;
        }
        pending.emplace_back(&child.value(), item.second + 1);
      }
    }
    if (bytes > kMaxBytes) {
      why = "JSON 超过 1 MiB";
      return false;
    }
  }
  try {
    if (json.dump().size() > kMaxBytes) {
      why = "JSON 超过 1 MiB";
      return false;
    }
  } catch (const nlohmann::json::exception&) {
    why = "JSON 字符串不是有效 UTF-8";
    return false;
  }
  return true;
}

inline bool kindMatches(const std::string& kind, const nlohmann::json& value) {
  if (kind == "number")
    return safeNumber(value);
  if (kind == "boolean")
    return value.is_boolean();
  if (kind == "string")
    return value.is_string();
  return kind == "record" && value.is_object() && value.size() == 2 && value.contains("type") &&
         value["type"].is_string() && !value["type"].get_ref<const std::string&>().empty() &&
         value.contains("data") && value["data"].is_object();
}

inline bool validate(const Record& record, std::string& why) {
  const auto& data = record.data;
  const bool isList = record.type == kListType;
  if (record.type != kValueType && !isList) {
    why = "需要 std.Value.v1 或 std.List.v1，收到 " + record.type;
    return false;
  }
  const char* content = isList ? "items" : "value";
  if (!data.is_object() || data.size() != 3 || !data.contains("kind") ||
      !data["kind"].is_string() || !data.contains("unit") || !data["unit"].is_string() ||
      !data.contains(content)) {
    why = "值契约需要且仅允许 kind、unit 与 value/items 字段";
    return false;
  }
  const auto& kind = data["kind"].get_ref<const std::string&>();
  if (kind != "number" && kind != "boolean" && kind != "string" && kind != "record") {
    why = "未知值类型 " + kind;
    return false;
  }
  if (kind != "number" && !data["unit"].get_ref<const std::string&>().empty()) {
    why = "只有 number 可以声明单位";
    return false;
  }
  if (isList) {
    if (!data[content].is_array() || data[content].size() > kMaxItems) {
      why = "items 必须是最多 4096 项的数组";
      return false;
    }
    std::string recordType;
    for (const auto& item : data[content]) {
      if (!kindMatches(kind, item)) {
        why = "列表元素与 kind 不符";
        return false;
      }
      if (kind == "record") {
        const auto type = item["type"].get<std::string>();
        if (!recordType.empty() && recordType != type) {
          why = "记录列表的元素 type 必须一致";
          return false;
        }
        recordType = type;
      }
    }
  } else if (!kindMatches(kind, data[content])) {
    why = "value 与 kind 不符";
    return false;
  }
  return boundedJson(data, why);
}

inline Record makeValue(const std::string& kind, nlohmann::json value,
                        const std::string& unit = {}) {
  return Record{kValueType, {{"kind", kind}, {"value", std::move(value)}, {"unit", unit}}};
}

inline Record number(double value, const std::string& unit = {}) {
  return makeValue("number", value, unit);
}

inline Record boolean(bool value) { return makeValue("boolean", value); }

inline Record string(const std::string& value) { return makeValue("string", value); }

inline Record makeList(const std::string& kind, nlohmann::json items,
                       const std::string& unit = {}) {
  return Record{kListType, {{"kind", kind}, {"items", std::move(items)}, {"unit", unit}}};
}

inline bool readValue(const Data& data, const Record*& record, std::string& why) {
  record = data.asRecord();
  if (!record || record->type != kValueType) {
    why = "输入必须是 std.Value.v1；测量值请显式使用 value.from_measurement";
    return false;
  }
  return validate(*record, why);
}

inline bool readList(const Data& data, const Record*& record, std::string& why) {
  record = data.asRecord();
  if (!record || record->type != kListType) {
    why = "输入必须是 std.List.v1";
    return false;
  }
  return validate(*record, why);
}

inline bool readNumber(const Data& data, double& number, std::string& unit, std::string& why) {
  const Record* record = nullptr;
  if (!readValue(data, record, why))
    return false;
  if (record->data["kind"] != "number") {
    why = "输入必须是 number 值";
    return false;
  }
  number = record->data["value"].get<double>();
  unit = record->data["unit"].get<std::string>();
  return true;
}

} // namespace lyflow::values
