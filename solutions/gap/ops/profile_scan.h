#pragma once

#include <filesystem>

#include "profile_scene.h"
#include "lyflow/operator.h"

namespace lyflow::packs::gap::profile_scene {
struct ProfilePairOptions {
  std::string source = "dir";
  std::filesystem::path dir;
  std::filesystem::path primaryFile;
  std::filesystem::path secondaryFile;
  std::string primaryPrefix = "LaserProfile_L0_Master_";
  std::string secondaryPrefix = "LaserProfile_R1_Slave_";
  std::string layout = "sensor";
  std::string unit;
  std::string fileFrame;
  std::string sensorFrame;
  std::string handedness;
  std::string primarySourceId;
  std::string secondarySourceId;
};
struct LoadedProfilePair {
  Data primary;
  Data secondary;
  Record sourceInfo;
};
struct ReadScanOptions {
  std::string measurementFrame;
  bool removeOutliers = false;
  double outlierRadiusMm = .4;
  std::int64_t outlierNeighbors = 5;
  nlohmann::json guard;
  nlohmann::json origins;
};

Status loadProfilePair(const ProfilePairOptions& options, LoadedProfilePair& result,
                       ExecContext& context);
Status buildReadScan(const Data& sensorPrimary, const Data& sensorSecondary,
                     const ReadScanOptions& options, Data& scan, ExecContext& context);
void registerProfileScanOps(Registry& registry);
} // namespace lyflow::packs::gap::profile_scene
