#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "lyflow/kernel/profile_robust.h"
#include "lyflow/kernel/profile_features.h"

namespace robust = lyflow::kernel::profile_robust;

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

robust::Samples samples(const std::vector<robust::Point>& points) {
  return {points.data(), points.size(), false};
}

void checkIndices(const std::vector<int>& indices, std::size_t size, int invalid) {
  require(!indices.empty(), "Fit must report inliers");
  for (int index : indices)
    require(index >= 0 && static_cast<std::size_t>(index) < size && index != invalid,
            "Fit inlier does not address the original valid slot");
}

template <std::size_t N> bool finite(const std::array<float, N>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}
}

int main() {
  try {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<robust::Point> linePoints{{nan, nan, nan}};
    for (int i = 0; i < 96; ++i) {
      const float x = static_cast<float>(i) * 0.001F;
      linePoints.push_back({x, 0.25F * x + 0.01F, 0.0F});
    }
    linePoints.push_back({0.04F, -0.02F, 0.0F});
    const auto originalLine = linePoints;
    robust::LineFit line;
    robust::Line2DFitOptions lineOptions;
    lineOptions.distThresh = 0.00001F;
    require(robust::fitLine(samples(linePoints), line, lineOptions), "RANSAC line fit failed");
    require(finite(line.coefficients), "RANSAC line coefficients must be finite");
    checkIndices(line.inliers, linePoints.size(), 0);

    std::vector<robust::Point> horizontal{{nan, nan, nan}};
    for (int i = 0; i < 64; ++i)
      horizontal.push_back({static_cast<float>(i) * 0.001F, 0.015F, 0.0F});
    robust::LineFit axis;
    require(robust::fitAxisLine(samples(horizontal), false, axis, lineOptions),
            "Axis-constrained line fit failed");
    require(finite(axis.coefficients), "Axis-constrained line coefficients must be finite");
    checkIndices(axis.inliers, horizontal.size(), 0);

    std::vector<robust::Point> circlePoints{{nan, nan, nan}, {-0.03F, 0.04F, 0.0F}};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 96; ++i) {
      const double angle = static_cast<double>(i) * 2.0 * pi / 96.0;
      circlePoints.push_back({static_cast<float>(0.01 + 0.005 * std::cos(angle)),
                              static_cast<float>(0.02 + 0.005 * std::sin(angle)), 0.0F});
    }
    const auto originalCircle = circlePoints;
    robust::Circle2DFitOptions circleOptions;
    circleOptions.distThresh = 0.00001F;
    circleOptions.minRadius = 0.004;
    circleOptions.maxRadius = 0.006;
    robust::CircleFit circle;
    require(robust::fitCircle(samples(circlePoints), circle, circleOptions),
            "RANSAC circle fit failed");
    require(finite(circle.coefficients) && circle.coefficients[2] >= circleOptions.minRadius &&
                circle.coefficients[2] <= circleOptions.maxRadius,
            "RANSAC circle must be finite within its radius bounds");
    checkIndices(circle.inliers, circlePoints.size(), 0);

    robust::CircleFit fixed;
    require(robust::fitCircleFixedRadius(samples(circlePoints), 0.005, fixed, circleOptions),
            "Fixed-radius circle fit failed");
    require(finite(fixed.coefficients), "Fixed-radius circle must be finite");
    checkIndices(fixed.inliers, circlePoints.size(), 0);
    circleOptions.radiusPrior = 0.0052;
    circleOptions.radiusPriorSigma = 0.0001;
    robust::CircleFit prior;
    require(robust::fitCircle(samples(circlePoints), prior, circleOptions),
            "Radius-prior fit failed");
    require(prior.report.radius == robust::Circle2DRadiusOutcome::Prior,
            "Soft-radius prior was not applied");
    require(prior.coefficients[2] >= circleOptions.minRadius &&
                prior.coefficients[2] <= circleOptions.maxRadius,
            "Radius-prior fit escaped its bounds");

    require(std::memcmp(originalLine.data(), linePoints.data(),
                        linePoints.size() * sizeof(robust::Point)) == 0 &&
                std::memcmp(originalCircle.data(), circlePoints.data(),
                            circlePoints.size() * sizeof(robust::Point)) == 0,
            "Borrowed sample arrays were modified");
    const auto unchanged = line;
    bool invalid = false;
    try {
      robust::fitLine({nullptr, 3, false}, line, lineOptions);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    require(invalid, "Missing borrowed sample storage was accepted");
    require(!robust::fitLine({}, line, lineOptions), "Empty samples were accepted");
    require(line.coefficients == unchanged.coefficients && line.inliers == unchanged.inliers,
            "Failed fit changed the output");
    bool cancelled = false;
    try {
      robust::fitLine(samples(linePoints), line, lineOptions, [] { return true; });
    } catch (const robust::Cancelled&) {
      cancelled = true;
    }
    require(cancelled && line.coefficients == unchanged.coefficients &&
                line.inliers == unchanged.inliers,
            "Cancellation changed the output or was ignored");
    namespace features = lyflow::kernel::profile_features;
    features::LineOptions featureOptions;
    featureOptions.fit.distThresh = 0.00001F;
    featureOptions.segmentPoints = 12;
    featureOptions.toward = {0.1, 0.035};
    featureOptions.endpoints = features::Endpoints::InlierEnds;
    features::LineFeature feature;
    require(features::fitLine(samples(linePoints), featureOptions, feature),
            "Complete public line feature workflow failed");
    checkIndices(feature.inliers, linePoints.size(), 0);
    checkIndices({feature.innerSlot}, linePoints.size(), 0);
    require(feature.segmentApplied && feature.start[0] < feature.end[0] &&
                feature.quality.pointCount == linePoints.size(),
            "Line feature segment/endpoint/original-slot contract changed");
    std::vector<robust::Point> weightedPoints;
    for (int i = 0; i < 6; ++i)
      weightedPoints.push_back({i * 0.01F, i * 0.001F, 0});
    featureOptions.segmentPoints = 0;
    featureOptions.fit.distThresh = 1e-8F;
    featureOptions.directionMode = features::DirectionMode::Fixed;
    featureOptions.hasReference = true;
    featureOptions.referenceDirection = {1, 0};
    require(features::fitLine(samples(weightedPoints), featureOptions, feature),
            "Fixed direction feature workflow failed");
    checkIndices(feature.inliers, weightedPoints.size(), -1);
    require(finite(feature.coefficients) && feature.quality.inlierCount == feature.inliers.size(),
            "Final quality weights must count every fixed-direction inlier");
    const auto preservedFeature = feature;
    featureOptions.minInliers = 100;
    require(!features::fitLine(samples(weightedPoints), featureOptions, feature) &&
                feature.coefficients == preservedFeature.coefficients &&
                feature.inliers == preservedFeature.inliers,
            "Rejected feature quality gate changed caller-owned output");
    cancelled = false;
    try {
      features::fitLine(samples(weightedPoints), featureOptions, feature, nullptr,
                        [] { return true; });
    } catch (const robust::Cancelled&) {
      cancelled = true;
    }
    require(cancelled && feature.coefficients == preservedFeature.coefficients &&
                feature.inliers == preservedFeature.inliers,
            "Cancelled line feature workflow changed caller-owned output");
    std::cout << "{\"type\":\"lyflow.ProfileRobustSdkAcceptance.v1\",\"fitMethods\":4,"
                 "\"radiusPrior\":true,\"inputSlotsPreserved\":true,\"arraysReadonly\":true,"
                 "\"invalidOutputAtomic\":true,\"cancelOutputAtomic\":true,"
                 "\"profileFeatures\":true,\"weightedInliersPreserved\":true,"
                 "\"featureOutputAtomic\":true,\"lineInliers\":"
              << line.inliers.size() << ",\"circleInliers\":" << circle.inliers.size() << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
