#include "gap_core/MeasurementEngine.hpp"

#include "gap_detection/GapDetection.hpp"

#include <pcl/common/point_tests.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace gap::core {
namespace {

Failure validateCloud(const PointCloud& cloud, const std::string& component) {
  if (cloud.empty()) {
    return {FailureStage::kInput, FailureCode::kEmptyCloud, component,
            component + " cloud is empty"};
  }
  for (const auto& point : cloud) {
    if (!pcl::isFinite(point)) {
      return {FailureStage::kInput, FailureCode::kNonFiniteValue, component,
              component + " cloud contains a non-finite point"};
    }
  }
  return {};
}

PointCloud removeNonFinitePoints(const PointCloud& cloud) {
  PointCloud result;
  result.header = cloud.header;
  result.sensor_origin_ = cloud.sensor_origin_;
  result.sensor_orientation_ = cloud.sensor_orientation_;
  result.points.reserve(cloud.size());
  for (const auto& point : cloud) {
    if (pcl::isFinite(point)) result.points.push_back(point);
  }
  result.width = static_cast<std::uint32_t>(result.points.size());
  result.height = 1;
  result.is_dense = true;
  return result;
}

MeasurementValue inactiveValue() {
  MeasurementValue value;
  value.status = MeasurementStatus::kInactive;
  return value;
}

MeasurementValue failedValue(Failure failure) {
  MeasurementValue value;
  value.status = MeasurementStatus::kFailure;
  value.failure = std::move(failure);
  return value;
}

MeasurementValue successfulValue(double value_mm, const std::string& component) {
  if (!std::isfinite(value_mm)) {
    return failedValue({FailureStage::kMeasurement, FailureCode::kInvalidGeometry, component,
                        component + " measurement is not finite"});
  }
  MeasurementValue value;
  value.status = MeasurementStatus::kSuccess;
  value.value_mm = value_mm;
  return value;
}

}  // namespace

MeasurementResult MeasurementEngine::measure(const MeasurementRequest& request) const noexcept {
  const auto total_start = std::chrono::steady_clock::now();
  MeasurementResult result;
  result.sample_id = request.sample_id;
  result.algorithm_id = request.options.algorithm_id;
  result.gap = request.configuration.gap.enabled ? MeasurementValue{} : inactiveValue();
  result.flush = request.configuration.flush.enabled ? MeasurementValue{} : inactiveValue();
  result.quality.input_point_count = request.primary_cloud.size() + request.secondary_cloud.size();

  PointCloud primary_cloud = request.primary_cloud;
  PointCloud secondary_cloud = request.secondary_cloud;
  if (request.options.non_finite_point_policy == NonFinitePointPolicy::kRemove) {
    primary_cloud = removeNonFinitePoints(primary_cloud);
    secondary_cloud = removeNonFinitePoints(secondary_cloud);
  }

  const auto failGlobally = [&](Failure failure) {
    result.failure = failure;
    if (request.configuration.flush.enabled) result.flush = failedValue(failure);
    if (request.configuration.gap.enabled) result.gap = failedValue(failure);
  };

  try {
    if (!request.configuration.flush.enabled && !request.configuration.gap.enabled) {
      failGlobally({FailureStage::kInput, FailureCode::kInvalidConfiguration, "configuration",
                    "at least one of gap or flush must be enabled"});
    } else if (const auto failure = validateCloud(primary_cloud, "primary"); failure) {
      failGlobally(failure);
    } else if (const auto failure = validateCloud(secondary_cloud, "secondary"); failure) {
      failGlobally(failure);
    } else {
      detection::GapDetection detector;
      detector.setConfiguration(request.configuration);
      detector.no_trans_ = request.options.input_frame == PointCloudFrame::kMeasurementXY;
      detector.enableDiagnosticMode(request.options.collect_partial_results);
      detector.enableFitPointsExport(request.options.export_fit_points);

      try {
        detector.detect(primary_cloud, secondary_cloud);
      } catch (const std::exception& error) {
        const Failure failure{detector.currentStage(), classifyFailureCode(error.what()),
                              "pipeline", error.what()};
        detector.finishDiagnostics();
        failGlobally(failure);
      }

      result.quality = detector.qualityMetrics();
      result.quality.icp = detector.icpMetrics();
      result.quality.point_counts["input_primary_removed_non_finite"] =
          request.primary_cloud.size() - primary_cloud.size();
      result.quality.point_counts["input_secondary_removed_non_finite"] =
          request.secondary_cloud.size() - secondary_cloud.size();

      if (!result.failure) {
        if (request.configuration.flush.enabled) {
          result.flush = detector.flushFailure()
                             ? failedValue(detector.flushFailure())
                             : successfulValue(detector.flushMeasurementMm(), "flush");
        }
        if (request.configuration.gap.enabled) {
          result.gap = detector.gapFailure() ? failedValue(detector.gapFailure())
                                             : successfulValue(detector.gapMeasurementMm(), "gap");
        }
        if (result.flush.status == MeasurementStatus::kFailure) {
          result.failure = result.flush.failure;
        } else if (result.gap.status == MeasurementStatus::kFailure) {
          result.failure = result.gap.failure;
        }
      }

      if (request.options.capture_geometry) {
        result.geometry.left_cloud = detector.leftCloud();
        result.geometry.right_cloud = detector.rightCloud();
        result.geometry.gap_points = detector.gapPoints();
        result.geometry.flush_points = detector.flushPoints();
        result.geometry.rois = detector.rois();
        result.geometry.lines = detector.lines();
        result.geometry.circles = detector.circles();
      }
    }
  } catch (const std::exception& error) {
    failGlobally(
        {FailureStage::kMeasurement, FailureCode::kUnexpectedError, "engine", error.what()});
  } catch (...) {
    failGlobally({FailureStage::kMeasurement, FailureCode::kUnexpectedError, "engine",
                  "unknown measurement error"});
  }

  const auto enabledSucceeded = [](bool enabled, const MeasurementValue& value) {
    return !enabled || value.status == MeasurementStatus::kSuccess;
  };
  result.success = !result.failure &&
                   enabledSucceeded(request.configuration.flush.enabled, result.flush) &&
                   enabledSucceeded(request.configuration.gap.enabled, result.gap);
  result.quality.total_runtime_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - total_start)
                                        .count();
  if (result.quality.runtime_us == 0) result.quality.runtime_us = result.quality.total_runtime_us;
  return result;
}

}  // namespace gap::core
