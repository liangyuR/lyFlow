#pragma once

#include <array>
#include <string>

#include "lyflow/data.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap::profile_scene {
inline constexpr const char* kScanPairKind = "gap.ScanPair.v3";
inline constexpr const char* kRoiSetKind = "gap.RoiSet.v3";
inline constexpr const char* kLabelRowsKind = "gap.LabelRows.v3";
inline constexpr const char* kScanInfoType = "gap.ScanInfo.v3";
inline constexpr const char* kRoiInfoType = "gap.RoiInfo.v3";

struct ScanMetadata {
  std::string sensorFrame;
  std::string measurementFrame;
  std::string stage;
  nlohmann::json guard;
  nlohmann::json crop;
  nlohmann::json origins;
  nlohmann::json processing;
};

struct ScanView {
  const Data* sensorPrimary = nullptr;
  const Data* sensorSecondary = nullptr;
  const Data* primary = nullptr;
  const Data* secondary = nullptr;
  const Data* merged = nullptr;
  const Record* info = nullptr;
  const Data* pick(const std::string& role) const;
};

bool makeScanPair(const Data& sensorPrimary, const Data& sensorSecondary, const Data& primary,
                  const Data& secondary, const Data& merged, const ScanMetadata& metadata,
                  Data& result, std::string& why);
bool readScanPair(const Data& data, ScanView& result, std::string& why);

struct RoiFields {
  std::array<Data, 4> boxes;
  std::array<Data, 4> contexts;
  Record info;
};

struct RoiView {
  std::array<const Data*, 4> boxes{};
  std::array<const Data*, 4> contexts{};
  const Record* info = nullptr;
  const Data* role(const std::string& name) const;
  const Data* context(const std::string& name) const;
};

bool makeRoiSet(const RoiFields& fields, Data& result, std::string& why);
bool readRoiSet(const Data& data, RoiView& result, std::string& why);
void registerProfileBundles(Registry& registry);
} // namespace lyflow::packs::gap::profile_scene
