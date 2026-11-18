#pragma once

#include <array>

namespace detection::domain {

enum class InitialPoseMode {
  kEndpointMidpointTranslation = 0,
  kCentroidTranslation = 1,
  kPcaRigidCentroid = 2,
};

struct Roi {
  std::array<double, 4> values{};
};

struct TemplateRoiConfiguration {
  Roi flush_base;
  Roi gap_left;
  Roi flush_ref;
  Roi gap_right;
};

struct IcpConfiguration {
  double max_matching_distance = 0;
  double max_fitness_distance = 0;
  int max_iteration_count = 0;
  int minimum_score = 0;
  int neighbor_count = 0;
  bool bidirectional = false;
};

} // namespace detection::domain
