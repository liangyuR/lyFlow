#pragma once

#include <limits>
#include <set>
#include <string>
#include <vector>

#include "lyflow/data.h"
#include "lyflow/value.h"

namespace lyflow::cloud_groups {

inline constexpr const char* kType = "cloud.Groups.v1";
inline constexpr std::size_t kMaxGroups = 4096;

inline bool nonnegativeInteger(const nlohmann::json& value) {
  return value.is_number_integer() &&
         (value.is_number_unsigned() || value.get<std::int64_t>() >= 0);
}

struct Group {
  std::string id;
  std::size_t offset = 0;
  std::size_t count = 0;
};

inline Record make(const PointCloud& cloud, const std::vector<Group>& groups,
                   const std::string& frame) {
  Record record;
  record.type = kType;
  record.data = {{"sourceCloudId", std::to_string(cloud.id)},
                 {"pointCount", cloud.pointCount()},
                 {"frame", frame},
                 {"unit", "m"},
                 {"groups", nlohmann::json::array()}};
  for (const auto& group : groups)
    record.data["groups"].push_back(
        {{"id", group.id}, {"offset", group.offset}, {"count", group.count}});
  return record;
}

inline bool parse(const Record& record, const PointCloud& cloud, const Indices& indices,
                  std::vector<Group>& groups, std::string& frame, std::string& why) {
  groups.clear();
  if (record.type != kType || !values::boundedJson(record.data, why)) {
    why = "需要有界 cloud.Groups.v1 元数据：" + why;
    return false;
  }
  const auto& data = record.data;
  if (!data.is_object() || data.size() != 5 || !data.contains("sourceCloudId") ||
      data["sourceCloudId"] != std::to_string(cloud.id) || !data.contains("pointCount") ||
      !nonnegativeInteger(data["pointCount"]) ||
      data["pointCount"].get<std::size_t>() != cloud.pointCount() || !data.contains("unit") ||
      data["unit"] != "m" || !data.contains("frame") || !data["frame"].is_string() ||
      data["frame"].get_ref<const std::string&>().empty() ||
      data["frame"].get_ref<const std::string&>().size() > 1024 || !data.contains("groups") ||
      !data["groups"].is_array() || data["groups"].size() > kMaxGroups ||
      indices.sourceCloudId != cloud.id) {
    why = "分组类型、来源点云、帧、单位或组数不匹配";
    return false;
  }
  std::size_t expected = 0;
  std::set<std::string> ids;
  for (const auto& item : data["groups"]) {
    if (!item.is_object() || item.size() != 3 || !item.contains("id") || !item["id"].is_string() ||
        item["id"].get_ref<const std::string&>().empty() || !item.contains("offset") ||
        !nonnegativeInteger(item["offset"]) || !item.contains("count") ||
        !nonnegativeInteger(item["count"])) {
      why = "每组需要 id、无符号 offset/count";
      return false;
    }
    const std::string id = item["id"];
    const auto offset = item["offset"].get<std::size_t>();
    const auto count = item["count"].get<std::size_t>();
    if (!ids.insert(id).second || offset != expected || offset > indices.values.size() ||
        count > indices.values.size() - offset) {
      why = "分组身份重复、区间不连续或索引区间越界";
      return false;
    }
    groups.push_back({id, offset, count});
    expected += count;
  }
  if (expected != indices.values.size()) {
    why = "分组区间必须完整覆盖原生索引";
    return false;
  }
  for (auto index : indices.values) {
    if (index < 0 || static_cast<std::size_t>(index) >= cloud.pointCount()) {
      why = "分组中的原生索引越界";
      return false;
    }
  }
  frame = data["frame"];
  return true;
}

} // namespace lyflow::cloud_groups
