#pragma once

#include "domain/detection/DetectionConfiguration.hpp"

#include <yaml-cpp/yaml.h>

#include <string>

namespace detection {

domain::AlignmentConfiguration parseAlignmentConfiguration(const YAML::Node& node);
domain::DetectionConfiguration parseDetectionConfiguration(const YAML::Node& node);
domain::CalibrationConfiguration parseCalibrationConfiguration(const YAML::Node& node);
domain::CalibrationConfiguration loadCalibrationConfiguration(const std::string& path);

}  // namespace detection
