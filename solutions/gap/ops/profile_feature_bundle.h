#pragma once
#include <array>
#include <string>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap::profile_scene {
inline constexpr const char* kLineFeatureKind = "gap.LineFeature.v2";
inline constexpr const char* kReferenceFeatureKind = "gap.ReferenceFeature.v2";
inline constexpr const char* kCirclePairKind = "gap.CirclePair.v2";
inline constexpr const char* kLocationKind = "gap.Location.v2";
struct LineFeatureFields {
  Data profile, line, lineContext, innerEnd, pointContext;
  Data inliers, weightedInliers, quality, provenance;
};
struct ReferenceFeatureFields {
  Data profile, line, lineContext, point, pointContext, selection, quality, provenance;
};
struct CirclePairFeatureFields {
  std::array<Data, 2> profiles, circles, contexts, provenances, inliers;
  Record quality;
};
struct LocationFields {
  Data scan, rois;
};
bool makeLineFeature(const LineFeatureFields&, Data& result, std::string& why);
bool readLineFeature(const Data&, LineFeatureFields& result, std::string& why);
bool makeReferenceFeature(const ReferenceFeatureFields&, Data& result, std::string& why);
bool readReferenceFeature(const Data&, ReferenceFeatureFields& result, std::string& why);
bool completeReferenceFeature(const std::string& method, const ReferenceFeatureFields& partial,
                              ReferenceFeatureFields& result, std::string& why);
bool makeCirclePairFeature(const CirclePairFeatureFields&, Data& result, std::string& why);
bool readCirclePairFeature(const Data&, CirclePairFeatureFields& result, std::string& why);
bool makeLocation(const Data& scan, const Data& rois, Data& result, std::string& why);
bool readLocation(const Data&, LocationFields& result, std::string& why);
void registerProfileFeatureBundles(Registry&);
}
