#include "lyflow/profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace lyflow {
namespace {
bool fail(std::string& why, const char* message) {
  why = message;
  return false;
}
bool name(const std::string& value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos;
}
} // namespace

nlohmann::json profileDescriptorJson(const ProfileDescriptor& d) {
  return {{"schemaVersion", d.schemaVersion},
          {"frame", d.frame},
          {"unit", d.unit},
          {"plane", d.plane},
          {"handedness", d.handedness},
          {"sampleOrder", d.sampleOrder},
          {"sourceId", d.sourceId},
          {"sourceCount", d.sourceCount}};
}

bool validateProfileDescriptor(const ProfileDescriptor& d, std::string& why) {
  if (d.schemaVersion != 1)
    return fail(why, "Profile schemaVersion必须为1");
  if (!name(d.frame) || !name(d.sourceId))
    return fail(why, "Profile frame/sourceId必须为非空且不含NUL的UTF-8字符串，最多1024字节");
  if (d.unit != "m" && d.unit != "mm")
    return fail(why, "Profile unit必须显式声明为m或mm");
  if (d.plane != "XY" && d.plane != "XZ" && d.plane != "YZ")
    return fail(why, "Profile plane必须显式声明为XY/XZ/YZ");
  if (d.handedness != "right" && d.handedness != "reflected")
    return fail(why, "Profile handedness必须为right/reflected");
  if (d.sampleOrder != "unknown" && d.sampleOrder != "acquisition")
    return fail(why, "Profile sampleOrder必须为unknown/acquisition");
  if (d.sourceCount > std::numeric_limits<std::uint32_t>::max())
    return fail(why, "Profile sourceCount不得超过uint32上限");
  try {
    if (profileDescriptorJson(d).dump().size() > 8192)
      return fail(why, "Profile元数据超过8KiB");
  } catch (const nlohmann::json::exception&) {
    return fail(why, "Profile元数据必须为合法UTF-8");
  }
  why.clear();
  return true;
}

bool parseProfileDescriptor(const nlohmann::json& value, ProfileDescriptor& out, std::string& why) {
  static const std::set<std::string> keys{"schemaVersion", "frame",      "unit",
                                          "plane",         "handedness", "sampleOrder",
                                          "sourceId",      "sourceCount"};
  if (!value.is_object() || value.size() != keys.size())
    return fail(why, "Profile元数据必须包含完整且唯一的版本1字段");
  for (const auto& item : value.items())
    if (!keys.count(item.key()))
      return fail(why, "Profile元数据含未知字段");
  try {
    if (!value.at("schemaVersion").is_number_integer() || value.at("schemaVersion") != 1 ||
        !value.at("sourceCount").is_number_integer() || value.at("sourceCount") < 0 ||
        value.at("sourceCount") > std::numeric_limits<std::uint32_t>::max())
      return fail(why, "Profile schemaVersion/sourceCount必须为受支持的非负整数");
    ProfileDescriptor d;
    d.schemaVersion = 1;
    d.frame = value.at("frame").get<std::string>();
    d.unit = value.at("unit").get<std::string>();
    d.plane = value.at("plane").get<std::string>();
    d.handedness = value.at("handedness").get<std::string>();
    d.sampleOrder = value.at("sampleOrder").get<std::string>();
    d.sourceId = value.at("sourceId").get<std::string>();
    d.sourceCount = value.at("sourceCount").get<std::uint64_t>();
    if (!validateProfileDescriptor(d, why))
      return false;
    out = std::move(d);
    return true;
  } catch (const nlohmann::json::exception&) {
    return fail(why, "Profile元数据字段类型无效");
  }
}

bool validateProfile(const Profile& p, std::string& why) {
  if (!validateProfileDescriptor(p.descriptor, why))
    return false;
  if (!p.cloud || !p.cloud->channelsConsistent())
    return fail(why, "Profile必须持有通道长度一致的点云缓冲");
  const auto n = p.pointCount();
  if (n > p.descriptor.sourceCount)
    return fail(why, "Profile槽数量不能超过sourceCount");
  if (p.validity.size() != n || p.sourceIndices.size() != n)
    return fail(why, "Profile validity/sourceIndices必须与原始槽数量完全一致");
  if (p.descriptor.sampleOrder == "unknown") {
    auto indices = p.sourceIndices;
    std::sort(indices.begin(), indices.end());
    if (std::adjacent_find(indices.begin(), indices.end()) != indices.end())
      return fail(why, "Profile sourceIndices不得重复");
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (p.validity[i] > 1)
      return fail(why, "Profile validity只能为0或1");
    const auto index = p.sourceIndices[i];
    if (index >= p.descriptor.sourceCount)
      return fail(why, "Profile sourceIndices越界");
    if (p.descriptor.sampleOrder == "acquisition" && i && index <= p.sourceIndices[i - 1])
      return fail(why, "acquisition轮廓的sourceIndices必须严格递增");
    for (std::size_t j = 0; j < 3; ++j) {
      const auto v = p.cloud->xyz[i * 3 + j];
      if (std::isinf(v) || (p.validity[i] && !std::isfinite(v)))
        return fail(why, "Profile有效槽必须为有限坐标，无效槽允许NaN但不允许Inf");
    }
  }
  why.clear();
  return true;
}

bool Profile::consistent(std::string* why) const {
  std::string message;
  const bool result = validateProfile(*this, message);
  if (why)
    *why = std::move(message);
  return result;
}

bool makeProfile(std::shared_ptr<const PointCloud> cloud, ProfileDescriptor descriptor,
                 std::vector<std::uint8_t> validity, std::vector<std::uint32_t> sourceIndices,
                 Profile& out, std::string& why) {
  Profile profile{std::move(cloud), std::move(descriptor), std::move(validity),
                  std::move(sourceIndices)};
  if (!validateProfile(profile, why))
    return false;
  out = std::move(profile);
  return true;
}

bool convertProfileUnit(const Profile& input, const std::string& targetUnit, Profile& out,
                        std::string& why, const std::function<bool()>& cancelled) {
  if (cancelled && cancelled())
    return fail(why, "Profile单位转换已取消");
  if (!validateProfile(input, why))
    return false;
  if (targetUnit != "m" && targetUnit != "mm")
    return fail(why, "Profile目标单位必须为m/mm");
  Profile result = input;
  if (targetUnit != input.descriptor.unit) {
    PointCloud cloud = *input.cloud;
    PointCloud identity;
    cloud.id = identity.id;
    const double scale = targetUnit == "m" ? 0.001 : 1000.0;
    for (std::size_t i = 0; i < cloud.xyz.size(); ++i) {
      if (i % (4096 * 3) == 0 && cancelled && cancelled())
        return fail(why, "Profile单位转换已取消");
      auto& value = cloud.xyz[i];
      if (std::isnan(value))
        continue;
      const double scaled = static_cast<double>(value) * scale;
      if (!std::isfinite(scaled) || std::abs(scaled) > std::numeric_limits<float>::max())
        return fail(why, "Profile单位转换后的坐标超出float32范围");
      value = static_cast<float>(scaled);
    }
    result.cloud = std::make_shared<const PointCloud>(std::move(cloud));
    result.descriptor.unit = targetUnit;
  }
  if (cancelled && cancelled())
    return fail(why, "Profile单位转换已取消");
  out = std::move(result);
  why.clear();
  return true;
}
} // namespace lyflow
