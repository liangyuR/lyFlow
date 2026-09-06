/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */

#pragma once
#include "domain/detection/DetectionConfiguration.hpp"
#include "gap_core/MeasurementTypes.hpp"
#include "gap_detection/Alignment.hpp"

#include <array>
#include <chrono>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace detection {

class GapDetection : public Alignment {
 public:
  GapDetection() = default;

  void setParams(const YAML::Node &params);

  void setConfiguration(const domain::DetectionConfiguration &configuration);

  // Input cloud must be on x-y plane, the return value is the execution time of this function
  int64_t detect(const PointCloud &left_camera_cloud, const PointCloud &right_camera_cloud);

  int64_t run(const PointCloud &left_camera_cloud, const PointCloud &right_camera_cloud) override;

  std::string getFlush() const;

  std::string getGap() const;

  std::string getFlushNominal() const;

  std::string getGapNominal() const;

  std::string getLeftRadius() const;

  std::string getRightRadius() const;

  double flushMeasurementMm() const;

  double gapMeasurementMm() const;

  void enableDiagnosticMode(bool enabled) { diagnostic_mode_ = enabled; }

  // Off by default: gates FitQuality::roi_points_mm/inlier_mask, which are the only
  // per-point (i.e. not O(1)) additions this ML export adds to QualityMetrics.
  void enableFitPointsExport(bool enabled) { export_fit_points_ = enabled; }

  const gap::core::Failure &flushFailure() const { return flush_failure_; }

  const gap::core::Failure &gapFailure() const { return gap_failure_; }

  gap::core::FailureStage currentStage() const { return current_stage_; }

  const gap::core::QualityMetrics &qualityMetrics() const { return quality_; }

  void finishDiagnostics();

  const std::vector<gap::core::IcpQuality> &icpMetrics() const { return alignmentMetrics(); }

  const PointCloud &leftCloud() const { return *left_cloud_; }

  const PointCloud &rightCloud() const { return *right_cloud_; }

  const std::vector<Eigen::Vector2f> &gapPoints() const { return gap_pts_; }

  const std::vector<Eigen::Vector2f> &flushPoints() const { return flush_pts_; }

  const std::vector<Eigen::Matrix2f> &rois() const { return rois_; }

  const std::unordered_map<std::string, Eigen::Matrix2f> &lines() const { return lines_; }

  const std::unordered_map<std::string, Eigen::VectorXf> &circles() const { return circles_; }

  const domain::VisualizationConfiguration &visualizationConfiguration() const {
    return configuration_.visualization;
  }

 private:
  bool align_cloud_ = true;
  domain::DetectionConfiguration configuration_;
  std::vector<Eigen::Vector2f> gap_pts_;
  std::vector<Eigen::Vector2f> flush_pts_;
  std::vector<Eigen::Matrix2f> rois_;
  std::array<Eigen::Vector2f, 2> compensation_centers_{};

  std::unordered_map<std::string, Eigen::Matrix2f> lines_;
  std::unordered_map<std::string, Eigen::VectorXf> circles_;
  bool diagnostic_mode_ = false;
  gap::core::FailureStage current_stage_ = gap::core::FailureStage::kNone;
  std::string current_component_;
  gap::core::Failure flush_failure_;
  gap::core::Failure gap_failure_;
  gap::core::QualityMetrics quality_;
  bool export_fit_points_ = false;
  std::chrono::steady_clock::time_point stage_started_at_ = std::chrono::steady_clock::now();
  static std::unordered_map<std::string, std::string> type_map_;
  static std::unordered_set<std::string> no_filter_types_;
  // get distance between two points in mm (input: m)
  static double getMeasurment(const std::vector<Eigen::Vector2f> &two_point);

  double detectFlush(const PointCloud &flush_base_cloud, const PointCloud &flush_ref_cloud);

  // gap value is always >= 0
  double detectGap(const PointCloud &gap_left_cloud, const PointCloud &gap_right_cloud);

  void clear();

  void setStage(gap::core::FailureStage stage, std::string component = {});

  gap::core::Failure makeFailure(const std::string &component, const std::exception &error) const;

  void recordLineQuality(const std::string &component, const PointCloud &cloud,
                         const Eigen::VectorXf &line, const pcl::Indices &inliers);

  void recordCircleQuality(const std::string &component, const PointCloud &cloud,
                           const Eigen::VectorXf &circle, const pcl::Indices &inliers,
                           std::string model = "circle", std::string radius_mode = "");

  // Shared by both record*Quality functions; no-op unless export_fit_points_ is set.
  void fillMlExportFields(gap::core::FitQuality &metric, const PointCloud &cloud,
                          const pcl::Indices &inliers) const;

  std::string getRadius(bool is_left) const;
};

}  // namespace detection
