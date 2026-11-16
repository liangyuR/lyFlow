#pragma once

#include "lyflow/data.h"
#include <array>
#include <functional>

namespace lyflow {
nlohmann::json profileDescriptorJson(const ProfileDescriptor& descriptor);
bool parseProfileDescriptor(const nlohmann::json& value, ProfileDescriptor& out, std::string& why);
bool validateProfileDescriptor(const ProfileDescriptor& descriptor, std::string& why);
bool validateProfile(const Profile& profile, std::string& why);
bool makeProfile(std::shared_ptr<const PointCloud> cloud, ProfileDescriptor descriptor,
                 std::vector<std::uint8_t> validity, std::vector<std::uint32_t> sourceIndices,
                 Profile& out, std::string& why);
bool convertProfileUnit(const Profile& input, const std::string& targetUnit, Profile& out,
                        std::string& why, const std::function<bool()>& cancelled = {});
bool remapProfileAxes(const Profile& input, const std::string& axes,
                      const std::array<float, 3>& signs, const std::string& targetFrame,
                      Profile& out, std::string& why, const std::function<bool()>& cancelled = {});
} // namespace lyflow
