#pragma once

#include <stdexcept>

#include "lyflow/c_api.h"
#include "lyflow/profile.h"

inline const void* lyflow_profile_input_owner() {
  static const int owner = 0;
  return &owner;
}

struct lyflow_profile_input {
  const void* owner = lyflow_profile_input_owner();
  lyflow::Data data;
};

namespace lyflow::runtime {
inline Data copyProfileInput(const lyflow_run_profile_input& in) {
  if (in.handle) {
    if (in.handle->owner != lyflow_profile_input_owner())
      throw std::invalid_argument("Profile input belongs to another runtime");
    return in.handle->data;
  }
  const auto n = static_cast<std::size_t>(in.count);
  if (!in.descriptor_json || (n && (!in.xyz || !in.validity || !in.source_indices)))
    throw std::invalid_argument("Profile input buffers or descriptor are null");
  const auto invalid = [&](const std::string& message) {
    return Data::error(Status::Error(Phase::Validate, "bad_input", "Profile: " + message, {},
                                     in.port ? in.port : ""));
  };
  std::size_t metadataBytes = 0;
  while (metadataBytes <= 8192 && in.descriptor_json[metadataBytes])
    ++metadataBytes;
  if (metadataBytes > 8192)
    return invalid("descriptor exceeds 8KiB");
  Profile profile;
  std::string why;
  if (!parseProfileDescriptor(nlohmann::json::parse(in.descriptor_json), profile.descriptor, why))
    return invalid(why);
  if (n > profile.descriptor.sourceCount)
    return invalid("slot count exceeds sourceCount");
  auto cloud = std::make_shared<PointCloud>();
  if (n) {
    cloud->xyz.assign(in.xyz, in.xyz + 3 * n);
    if (in.intensity)
      cloud->intensity.assign(in.intensity, in.intensity + n);
    if (in.normals)
      cloud->normals.assign(in.normals, in.normals + 3 * n);
    if (in.rgb)
      cloud->rgb.assign(in.rgb, in.rgb + 3 * n);
    profile.validity.assign(in.validity, in.validity + n);
    profile.sourceIndices.assign(in.source_indices, in.source_indices + n);
  }
  profile.cloud = std::move(cloud);
  if (!validateProfile(profile, why))
    return invalid(why);
  return Data::profile(std::move(profile));
}
}
