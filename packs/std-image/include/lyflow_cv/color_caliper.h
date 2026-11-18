#pragma once

#include "lyflow_cv/inspection.h"

namespace lyflow::std_image::inspection {

inline constexpr const char* kColorCaliperKind = "image.ColorCaliper.v1";
inline constexpr const char* kColorStationsType = "image.ColorCaliperStations.v1";

enum class CaliperSampleKind {
  Background,
  Weak,
  Signal,
  Unknown,
  Highlight,
  Dark,
  Outside,
  Roi,
  Image
};

CaliperSampleKind scoreKind(double score, double segmentMin);
bool informative(CaliperSampleKind kind);
bool outside(CaliperSampleKind kind);
const char* sampleCause(CaliperSampleKind kind, bool distinguishOutside);
double caliperOffset(double half, int sample);
std::pair<int, int> samplesBeyond(const line_scan::Run& run, double half, int count);
const char* boundaryCause(bool complete, int sample, const std::vector<CaliperSampleKind>& kinds,
                          bool distinguishOutside);
Json unknownSpans(const std::vector<CaliperSampleKind>& kinds, double half,
                  bool distinguishOutside);

void registerColorCaliper(Registry& r);

} // namespace lyflow::std_image::inspection
