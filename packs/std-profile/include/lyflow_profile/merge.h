#pragma once

#include <functional>

#include "lyflow_profile/provenance.h"

namespace lyflow::profile_contract {
struct MergeOptions {
  std::string roleA = "a";
  std::string roleB = "b";
  std::string channelPolicy = "require_equal";
};
struct MergeResult {
  Data profile;
  Record provenance;
  Record info;
};
inline bool mergeProfiles(const Data& aData, const Data& bData, const Record* aProvenance,
                          const Record* bProvenance, const MergeOptions& options,
                          MergeResult& result, std::string& why,
                          const std::function<bool()>& cancelled = {}) {
  const auto failure = [&](const std::string& message, const char* = "", const char* = "") {
    why = message;
    return false;
  };
  const auto stopped = [&] { return cancelled && cancelled(); };
  const auto* a = aData.asProfile();
  const auto* b = bData.asProfile();
  nlohmann::json sources;
  if (!profile_contract::profileSources(aData, aProvenance, sources, why))
    return failure(why, "aProvenance");
  if (!profile_contract::profileSources(bData, bProvenance, sources, why))
    return failure(why, "bProvenance");
  if (!profile_contract::sameSpace(a->descriptor, b->descriptor) ||
      a->descriptor.unit != b->descriptor.unit)
    return failure("合并轮廓必须处于相同坐标帧、轴平面、手性和单位", "b");
  if (a->descriptor.sourceCount + b->descriptor.sourceCount >
      std::numeric_limits<std::uint32_t>::max())
    return failure("合并原始索引空间超过uint32预算", "b");
  const auto policy = options.channelPolicy;
  const auto& ac = *a->cloud;
  const auto& bc = *b->cloud;
  const bool nonempty = a->pointCount() && b->pointCount();
  if (policy == "require_equal" && nonempty &&
      (ac.hasIntensity() != bc.hasIntensity() || ac.hasNormals() != bc.hasNormals() ||
       ac.hasRgb() != bc.hasRgb()))
    return failure("非空轮廓通道不一致；需要显式选择intersection才允许丢弃", "b");
  const auto retained = [&](bool av, bool bv) {
    return (av || bv) && (!a->pointCount() || av) && (!b->pointCount() || bv);
  };
  const bool intensity = retained(ac.hasIntensity(), bc.hasIntensity());
  const bool normals = retained(ac.hasNormals(), bc.hasNormals());
  const bool rgb = retained(ac.hasRgb(), bc.hasRgb());
  nlohmann::json dropped = nlohmann::json::array();
  if ((ac.hasIntensity() || bc.hasIntensity()) && !intensity)
    dropped.push_back("intensity");
  if ((ac.hasNormals() || bc.hasNormals()) && !normals)
    dropped.push_back("normals");
  if ((ac.hasRgb() || bc.hasRgb()) && !rgb)
    dropped.push_back("rgb");
  if (stopped())
    return failure("轮廓合并已取消", "a", "cancelled");
  PointCloud cloud;
  Profile combined;
  const auto total = a->pointCount() + b->pointCount();
  cloud.xyz.reserve(total * 3);
  if (intensity)
    cloud.intensity.reserve(total);
  if (normals)
    cloud.normals.reserve(total * 3);
  if (rgb)
    cloud.rgb.reserve(total * 3);
  combined.validity.reserve(total);
  combined.sourceIndices.reserve(total);
  for (const auto& entry :
       {std::make_pair(a, std::uint64_t(0)), std::make_pair(b, a->descriptor.sourceCount)}) {
    const auto& source = *entry.first;
    for (std::size_t i = 0; i < source.pointCount(); ++i) {
      if ((i & 1023U) == 0 && stopped())
        return failure("轮廓合并已取消", "a", "cancelled");
      const auto& c = *source.cloud;
      cloud.xyz.insert(cloud.xyz.end(), c.xyz.begin() + i * 3, c.xyz.begin() + (i + 1) * 3);
      if (intensity)
        cloud.intensity.push_back(c.intensity[i]);
      if (normals)
        cloud.normals.insert(cloud.normals.end(), c.normals.begin() + i * 3,
                             c.normals.begin() + (i + 1) * 3);
      if (rgb)
        cloud.rgb.insert(cloud.rgb.end(), c.rgb.begin() + i * 3, c.rgb.begin() + (i + 1) * 3);
      combined.validity.push_back(source.validity[i]);
      combined.sourceIndices.push_back(
          static_cast<std::uint32_t>(entry.second + source.sourceIndices[i]));
    }
  }
  combined.cloud = std::make_shared<const PointCloud>(std::move(cloud));
  Record provenance;
  if (!profile_contract::makeCompositeProvenance(aData, bData, aProvenance, bProvenance,
                                                 options.roleA, options.roleB, policy, combined,
                                                 provenance, why))
    return failure(why);
  MergeResult candidate;
  candidate.profile = Data::profile(std::move(combined));
  candidate.provenance = std::move(provenance);
  candidate.info = {"profile.MergeInfo.v1",
                    {{"channelPolicy", policy},
                     {"droppedChannels", dropped},
                     {"order", {"a", "b"}},
                     {"pointCount", total},
                     {"sourceCount", candidate.profile.asProfile()->descriptor.sourceCount}}};
  if (stopped())
    return failure("轮廓合并已取消");
  result = std::move(candidate);
  why.clear();
  return true;
}
} // namespace lyflow::profile_contract
