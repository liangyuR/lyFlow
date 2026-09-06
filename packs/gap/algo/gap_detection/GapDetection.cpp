/*
 * Copyright (c) XYZ Robotics Inc. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Author: jianming huang <jianming.huang@xyzrobotics.ai>, 2023/02/02
 */
#include "gap_detection/GapDetection.hpp"

#include "std_bridge.hpp"

#include <cmath>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <utility>
#include <pcl/common/point_tests.h>
#include <pcl/io/pcd_io.h>

namespace detection {
namespace {

bool contains(const std::string &value, const std::string &needle) {
  return value.find(needle) != std::string::npos;
}

std::string formatTwoDecimals(double value) {
  if (std::isnan(value)) return "NAN";
  if (std::isinf(value)) return value < 0 ? "-INF" : "INF";
  std::ostringstream output;
  output << std::fixed << std::setprecision(2) << value;
  return output.str();
}

// SE(2) delta between a reference pose and a candidate pose, both mapping template -> cloud:
// candidate = delta * reference. Used by the Layer 1 trust region and, via its inverse relation,
// composeDelta() below.
struct Se2Delta {
  Eigen::Vector2f translation = Eigen::Vector2f::Zero();
  float rotation_rad = 0.0F;
};

Se2Delta decomposeDelta(const Eigen::Matrix3f &reference, const Eigen::Matrix3f &candidate) {
  const Eigen::Matrix3f delta = candidate * reference.inverse();
  Se2Delta result;
  result.translation = delta.block<2, 1>(0, 2);
  result.rotation_rad = std::atan2(delta(1, 0), delta(0, 0));
  return result;
}

Eigen::Matrix3f composeDelta(const Eigen::Matrix3f &reference, const Eigen::Vector2f &translation,
                             float rotation_rad) {
  Eigen::Matrix3f delta = Eigen::Matrix3f::Identity();
  const float cosine = std::cos(rotation_rad);
  const float sine = std::sin(rotation_rad);
  delta(0, 0) = cosine;
  delta(0, 1) = -sine;
  delta(1, 0) = sine;
  delta(1, 1) = cosine;
  delta.block<2, 1>(0, 2) = translation;
  return delta * reference;
}

// Layer 1 trust region: clamp (never reject) a side's final transform back to a bound around the
// joint/global reference pose. Reports the pre-clamp deviation regardless of whether clamping
// was applied, for shadow-style diagnostics.
Eigen::Matrix3f applyTrustRegion(const Eigen::Matrix3f &reference, const Eigen::Matrix3f &transform,
                                 double scale, double max_translation_mm, double max_rotation_deg,
                                 double *raw_translation_mm, double *raw_rotation_deg,
                                 bool *clamped) {
  const auto delta = decomposeDelta(reference, transform);
  const double translation_mm = static_cast<double>(delta.translation.norm()) * scale;
  const double rotation_deg = static_cast<double>(delta.rotation_rad) * 180.0 / M_PI;
  *raw_translation_mm = translation_mm;
  *raw_rotation_deg = rotation_deg;
  *clamped = false;
  const bool translation_exceeds = translation_mm > max_translation_mm;
  const bool rotation_exceeds = std::fabs(rotation_deg) > max_rotation_deg;
  if (!translation_exceeds && !rotation_exceeds) return transform;

  *clamped = true;
  Eigen::Vector2f clamped_translation = delta.translation;
  if (translation_exceeds && translation_mm > 0) {
    clamped_translation *= static_cast<float>(max_translation_mm / translation_mm);
  }
  double clamped_rotation_deg = rotation_deg;
  if (rotation_exceeds) clamped_rotation_deg = std::copysign(max_rotation_deg, rotation_deg);
  const float clamped_rotation_rad = static_cast<float>(clamped_rotation_deg * M_PI / 180.0);
  return composeDelta(reference, clamped_translation, clamped_rotation_rad);
}

void removeNonFinitePoints(PointCloud *cloud) {
  PointCloud finite;
  finite.header = cloud->header;
  finite.sensor_origin_ = cloud->sensor_origin_;
  finite.sensor_orientation_ = cloud->sensor_orientation_;
  finite.points.reserve(cloud->size());
  for (const auto &point : *cloud) {
    if (pcl::isFinite(point)) finite.points.push_back(point);
  }
  finite.width = static_cast<std::uint32_t>(finite.points.size());
  finite.height = 1;
  finite.is_dense = true;
  *cloud = std::move(finite);
}

}  // namespace

void GapDetection::setConfiguration(const domain::DetectionConfiguration &configuration) {
  configuration_ = configuration;
  Alignment::setConfiguration(configuration.alignment);
}

void GapDetection::clear() {
  left_cloud_.reset(new PointCloud);
  right_cloud_.reset(new PointCloud);
  left_camera_cloud_.reset(new PointCloud);
  right_camera_cloud_.reset(new PointCloud);

  gap_pts_.clear();
  flush_pts_.clear();
  lines_.clear();
  circles_.clear();
  rois_.clear();
  flush_failure_ = {};
  gap_failure_ = {};
  quality_ = {};
  current_stage_ = gap::core::FailureStage::kNone;
  current_component_.clear();
  stage_started_at_ = std::chrono::steady_clock::now();
  resetAlignmentMetrics();
}

void GapDetection::setStage(gap::core::FailureStage stage, std::string component) {
  const auto now = std::chrono::steady_clock::now();
  if (current_stage_ != gap::core::FailureStage::kNone) {
    quality_.timings.push_back(
        {current_stage_, current_component_,
         std::chrono::duration_cast<std::chrono::microseconds>(now - stage_started_at_).count()});
  }
  current_stage_ = stage;
  current_component_ = std::move(component);
  stage_started_at_ = now;
}

void GapDetection::finishDiagnostics() { setStage(gap::core::FailureStage::kNone); }

gap::core::Failure GapDetection::makeFailure(const std::string &component,
                                             const std::exception &error) const {
  return {current_stage_, gap::core::classifyFailureCode(error.what()), component, error.what()};
}

int64_t GapDetection::run(const PointCloud &left_camera_cloud,
                          const PointCloud &right_camera_cloud) {
  auto start_time = std::chrono::system_clock::now();
  clear();
  quality_.input_point_count = left_camera_cloud.size() + right_camera_cloud.size();
  quality_.point_counts["input_primary"] = left_camera_cloud.size();
  quality_.point_counts["input_secondary"] = right_camera_cloud.size();
  setStage(gap::core::FailureStage::kPreprocess);
  // Model-first short-circuit (offline_batch.md, 方案A): a per-sample roi_override carries
  // absolute sensor-frame boxes derived from the raw profile. Neither the overall-roi crop nor
  // the ICP alignment contributes anything to that path -- the crop can only delete points the
  // override still needs (observed: roi_empty on displaced cars), and ICP can only fail the
  // sample before the override is even reached (observed: 53 icp_score_low with valid model
  // boxes). Both are skipped; ICP remains the template-fallback path's private concern.
  const bool override_short_circuit = configuration_.roi_override.has_value();
  // Roll-anchored overall crop: the short-circuit above leaves the cloud unbounded, so distant
  // ghost lines and debris survive into segmentation. The model's roll pair is the most reliably
  // labelled feature available here, so it re-establishes a window that follows the part. The
  // boxes are read before the base_side swap, where gap_left/gap_right still mean exactly the
  // model's left and right roll.
  const auto &roll_crop_configuration = configuration_.common.roll_anchored_crop;
  std::optional<OverallCropOverride> override_crop;
  if (roll_crop_configuration.enabled) {
    if (!override_short_circuit) {
      // The crop only exists to re-bound the model-first path. On the template/ICP path the
      // overall-roi crop and ICP still run, so there is nothing to re-establish -- but the point
      // is configured for a crop that silently never runs, which is worth reporting.
      quality_.crop_status = "skipped:no_model_roi";
    } else {
      const auto crop = utils::computeRollAnchoredCropMm(configuration_.roi_override->gap_left,
                                                         configuration_.roi_override->gap_right,
                                                         roll_crop_configuration);
      if (crop.box_mm) {
        override_crop = OverallCropOverride{*crop.box_mm, roll_crop_configuration.min_points_kept};
      } else {
        quality_.crop_status = std::string("rejected:") + crop.skip_reason;
      }
    }
  }
  preprocess(left_camera_cloud, right_camera_cloud, configuration_.common.roi_guided_segmentation,
             override_short_circuit, override_crop);
  if (override_short_circuit && roll_crop_configuration.enabled) {
    quality_.point_counts["roll_crop_applied"] = rollCropApplied() ? 1 : 0;
    quality_.point_counts["roll_crop_reverted"] = rollCropReverted() ? 1 : 0;
    // Only a derived window can be applied or reverted; a rejection was already recorded above.
    if (override_crop) {
      quality_.crop_status = rollCropReverted() ? "reverted:min_points" : "applied";
    }
  }
  quality_.preprocessed_point_count = left_camera_cloud_->size() + right_camera_cloud_->size();
  quality_.point_counts["preprocess_primary"] = left_camera_cloud_->size();
  quality_.point_counts["preprocess_secondary"] = right_camera_cloud_->size();
  const auto &common = configuration_.common;
  const auto &flush_configuration = configuration_.flush;
  const auto &gap_configuration = configuration_.gap;
  auto cluster_angle = common.cluster_angle;
  auto neighbors = common.neighbor_count;
  auto label_mode = common.label_mode;
  auto seg_together = common.segment_together;
  auto roi_guided_segmentation = common.roi_guided_segmentation;
  auto removal_enable = common.filter.enabled;
  auto save_template = common.save_template;
  auto success_guide = configuration_.success_guide;
  align_cloud_ = configuration_.align_cloud && !override_short_circuit;
  const bool filter_by_business_roi =
      align_cloud_ || roi_guided_segmentation || override_short_circuit;

  auto flush_enable = flush_configuration.enabled;
  const auto &flush_base = flush_configuration.base_side;
  const auto &flush_base_type = flush_configuration.base_type;
  const auto &flush_ref_type = flush_configuration.reference_type;
  auto gap_enable = gap_configuration.enabled;
  const auto &gap_left_type = gap_configuration.left_type;
  const auto &gap_right_type = gap_configuration.right_type;

  rois_.resize(4);
  // Shared by configure_business_rois below and by the roi_override application (after the ICP
  // transform block): fills rois_ from a {flush_base, gap_left, flush_ref, gap_right} quadruple,
  // applying the same base_side swap either way.
  const auto apply_business_rois = [&](std::vector<std::array<double, 4>> roi_double) {
    assert(roi_double.size() == 4);
    if (flush_base == "right") std::swap(roi_double[0], roi_double[2]);
    for (int i = 0; i < 4; i++) {
      rois_[i] << roi_double[i][0], roi_double[i][2], roi_double[i][1], roi_double[i][3];
      rois_[i] /= scale_;
    }
  };
  const auto configure_business_rois =
      [&](const std::optional<domain::TemplateRoiConfiguration> &template_rois) {
        apply_business_rois(
            {template_rois ? template_rois->flush_base.values : flush_configuration.base_roi.values,
             template_rois ? template_rois->gap_left.values : gap_configuration.left_roi.values,
             template_rois ? template_rois->flush_ref.values
                           : flush_configuration.reference_roi.values,
             template_rois ? template_rois->gap_right.values : gap_configuration.right_roi.values});
      };
  configure_business_rois(std::nullopt);
  quality_.roi_source = "config";
  compensation_centers_[0] = Eigen::Vector2f(gap_configuration.left_compensation.nominal_center_x,
                                             gap_configuration.left_compensation.nominal_center_y) /
                             scale_;
  compensation_centers_[1] =
      Eigen::Vector2f(gap_configuration.right_compensation.nominal_center_x,
                      gap_configuration.right_compensation.nominal_center_y) /
      scale_;

  // split each input cloud to left and right
  setStage(gap::core::FailureStage::kSegmentation);
  if (roi_guided_segmentation) {
    // ROI mode keeps every valid point from both cameras. The four business ROIs select the
    // measurement features later, so points visible to only one camera must not be discarded by
    // region-growing size thresholds or exact camera-overlap filtering.
    PointCloud cloud_both = *right_camera_cloud_;
    cloud_both += *left_camera_cloud_;
    pcl::copyPointCloud(cloud_both, *left_cloud_);
    pcl::copyPointCloud(cloud_both, *right_cloud_);
  } else if (seg_together) {
    // Match the C# view exactly: transformed secondary profile first, primary second.
    PointCloud cloud_both = *right_camera_cloud_;
    cloud_both += *left_camera_cloud_;
    utils::splitAndExtractCloud(cloud_both, left_cloud_.get(), right_cloud_.get(), neighbors,
                                cluster_angle);
    utils::getCloudsOverlap(*left_camera_cloud_, *right_cloud_, right_cloud_.get());
    utils::getCloudsOverlap(*right_camera_cloud_, *left_cloud_, left_cloud_.get());
  } else {
    utils::splitAndExtractCloud(*left_camera_cloud_, nullptr, right_cloud_.get(), neighbors,
                                cluster_angle);
    utils::splitAndExtractCloud(*right_camera_cloud_, left_cloud_.get(), nullptr, neighbors,
                                cluster_angle);
  }
  quality_.point_counts["segmentation_left"] = left_cloud_->size();
  quality_.point_counts["segmentation_right"] = right_cloud_->size();
  if (removal_enable) {
    setStage(gap::core::FailureStage::kFiltering);
    quality_.point_counts["filter_before_left"] = left_cloud_->size();
    quality_.point_counts["filter_before_right"] = right_cloud_->size();
    auto filter_radius = common.filter.radius / scale_;
    auto filter_neighbor = common.filter.neighbor_count;
    utils::removeRadiusOutlier(*left_cloud_, left_cloud_.get(), filter_radius, filter_neighbor);
    utils::removeRadiusOutlier(*right_cloud_, right_cloud_.get(), filter_radius, filter_neighbor);
    quality_.point_counts["filter_after_left"] = left_cloud_->size();
    quality_.point_counts["filter_after_right"] = right_cloud_->size();
  }
  if (left_cloud_->empty() || right_cloud_->empty()) {
    // vis origin in case of split failed
    pcl::copyPointCloud(*left_camera_cloud_, *left_cloud_);
    pcl::copyPointCloud(*right_camera_cloud_, *right_cloud_);
    throw std::runtime_error("Cloud Split Failed");
  }
  quality_.left_point_count = left_cloud_->size();
  quality_.right_point_count = right_cloud_->size();

  if (!align_cloud_) {
    // skip align cloud by set label_mode to false, and skip template saving.
    label_mode = true;
    save_template = false;
  }
  // in label mode, save template and skip
  save_template_path_ = common.save_template_path;
  std::filesystem::create_directories(save_template_path_);
  auto left_template_path = save_template_path_ + "/left_template.pcd";
  auto right_template_path = save_template_path_ + "/right_template.pcd";
  if (label_mode) {
    // double check, only in datamanger_dialog and set save_template as true
    // template model would be overwritten
    if (save_template) {
      try {
        pcl::io::savePCDFile(left_template_path, *left_cloud_, true);
        pcl::io::savePCDFile(right_template_path, *right_cloud_, true);
      } catch (...) {
      }
    }
  } else {  // load template and make registration
    setStage(gap::core::FailureStage::kTemplateLoading);
    struct TemplatePair {
      std::string id;
      PointCloud left;
      PointCloud right;
      std::optional<domain::TemplateRoiConfiguration> rois;
      std::size_t configuration_order = 0;
    };
    struct TemplatePairOutcome {
      std::string id;
      std::size_t configuration_order = 0;
      AlignmentOutcome left;
      AlignmentOutcome right;
      std::optional<domain::TemplateRoiConfiguration> rois;
      std::optional<std::size_t> left_metric;
      std::optional<std::size_t> right_metric;
      // Layer 1: the (possibly ICP-refined) joint initial pose both sides were measured against;
      // retained so the Layer 3 consistency-gate retry can reuse the same trust-region reference.
      Eigen::Matrix3f trust_region_reference = Eigen::Matrix3f::Identity();

      bool success() const { return left.success && right.success; }
    };

    const auto load_template_pair =
        [&](const std::string &id, const std::filesystem::path &left_path,
            const std::filesystem::path &right_path,
            std::optional<domain::TemplateRoiConfiguration> rois, std::size_t configuration_order) {
          TemplatePair pair;
          pair.id = id;
          pair.rois = std::move(rois);
          pair.configuration_order = configuration_order;
          if (pcl::io::loadPCDFile(left_path.string(), pair.left) < 0 ||
              pcl::io::loadPCDFile(right_path.string(), pair.right) < 0) {
            if (configuration_order == 0) throw std::runtime_error("template missing or empty");
            throw std::runtime_error("template missing or empty for candidate: " + id);
          }
          removeNonFinitePoints(&pair.left);
          removeNonFinitePoints(&pair.right);
          if (pair.left.empty() || pair.right.empty()) {
            if (configuration_order == 0) throw std::runtime_error("template missing or empty");
            throw std::runtime_error("template missing or empty for candidate: " + id);
          }
          return pair;
        };

    const auto &robustness = configuration_.alignment.robustness;

    // initial_pose: seed passed to this side's own ICP. trust_region_reference: the joint/global
    // pose the result is clamped against (Layer 1); independent of initial_pose so a
    // success_guide/consistency retry seeded from the other side's transform is still clamped
    // against the shared reference, not against whatever seeded this particular attempt.
    const auto evaluate_side = [&](const std::string &template_id, const std::string &component,
                                   const PointCloud &source, const PointCloud &target,
                                   const Eigen::Matrix3f &initial_pose,
                                   const Eigen::Matrix3f &trust_region_reference) {
      setAlignmentComponent(component);
      auto outcome = alignOutcome(source, target, initial_pose);
      gap::core::IcpQuality metric;
      metric.component = component;
      metric.template_id = template_id;
      metric.selected = false;
      if (outcome.success) {
        double raw_translation_mm = std::numeric_limits<double>::quiet_NaN();
        double raw_rotation_deg = std::numeric_limits<double>::quiet_NaN();
        bool clamped = false;
        outcome.transform = applyTrustRegion(trust_region_reference, outcome.transform, scale_,
                                             robustness.trust_region.max_translation_mm,
                                             robustness.trust_region.max_rotation_deg,
                                             &raw_translation_mm, &raw_rotation_deg, &clamped);
        metric.trust_region_delta_translation_mm = raw_translation_mm;
        metric.trust_region_delta_rotation_deg = raw_rotation_deg;
        metric.trust_region_clamped = clamped;
      }
      metric.score = outcome.score;
      metric.bidirectional = outcome.bidirectional;
      metric.success = outcome.success;
      metric.transform = outcome.transform;
      metric.degenerate_eigenvalue_ratio = outcome.degenerate_eigenvalue_ratio;
      metric.degenerate_locked = outcome.degenerate_locked;
      icp_metrics_.push_back(std::move(metric));
      return std::make_pair(outcome, icp_metrics_.size() - 1);
    };

    // Shared by the success_guide fallback and the Layer 3 consistency-gate retry: re-run one
    // side's ICP using the other (already-converged) side's transform as the initial pose.
    const auto retry_side_with_transform =
        [&](const std::string &template_id, const std::string &component, const PointCloud &source,
            const PointCloud &target, const Eigen::Matrix3f &other_side_transform,
            const Eigen::Matrix3f &trust_region_reference, AlignmentOutcome *outcome,
            std::optional<std::size_t> *metric_index) {
          auto retried = evaluate_side(template_id, component, source, target, other_side_transform,
                                       trust_region_reference);
          *outcome = retried.first;
          *metric_index = retried.second;
        };

    const auto evaluate_pair = [&](const TemplatePair &pair) {
      TemplatePairOutcome result;
      result.id = pair.id;
      result.rois = pair.rois;
      result.configuration_order = pair.configuration_order;
      const auto initial_pose = utils::computeInitialPose(
          pair.left, pair.right, *left_cloud_, *right_cloud_, configuration_.initial_pose_mode);

      // Layer 1, coarse stage: register the merged left+right template against the merged
      // left+right cloud to obtain a joint estimate, used as the initial pose for both sides and
      // as the trust-region reference. Falls back to the existing per-pair initial_pose (today's
      // behaviour) if disabled or if the joint registration itself fails.
      Eigen::Matrix3f side_initial_pose = initial_pose.transform;
      bool global_coarse_succeeded = false;
      if (robustness.global_coarse) {
        PointCloud merged_template(pair.left);
        merged_template += pair.right;
        PointCloud merged_target(*left_cloud_);
        merged_target += *right_cloud_;
        setAlignmentComponent("global");
        const auto global_outcome =
            alignOutcome(merged_template, merged_target, initial_pose.transform);
        gap::core::IcpQuality global_metric;
        global_metric.component = "global";
        global_metric.template_id = pair.id;
        global_metric.score = global_outcome.score;
        global_metric.bidirectional = global_outcome.bidirectional;
        global_metric.success = global_outcome.success;
        global_metric.transform = global_outcome.transform;
        global_metric.degenerate_eigenvalue_ratio = global_outcome.degenerate_eigenvalue_ratio;
        global_metric.degenerate_locked = global_outcome.degenerate_locked;
        global_metric.selected = false;
        icp_metrics_.push_back(std::move(global_metric));
        if (global_outcome.success) {
          side_initial_pose = global_outcome.transform;
          global_coarse_succeeded = true;
        }
      }
      quality_.point_counts[pair.id + "_global_coarse_attempted"] =
          robustness.global_coarse ? 1 : 0;
      quality_.point_counts[pair.id + "_global_coarse_succeeded"] = global_coarse_succeeded ? 1 : 0;
      result.trust_region_reference = side_initial_pose;

      auto left = evaluate_side(pair.id, "left", pair.left, *left_cloud_, side_initial_pose,
                                side_initial_pose);
      result.left = left.first;
      result.left_metric = left.second;
      // seg_mode ROI never splits, so both sides register against the same merged cloud. When the
      // template pair is mirrored too (one merged cloud stored to both files), the right side is
      // the identical registration problem: reuse the left outcome so both sides share one
      // transform and the business ROIs cannot drift relative to each other. A failed left side
      // still falls through to the retry paths below, which reseed with different initial poses.
      if (roi_guided_segmentation && result.left.success &&
          utils::cloudsIdentical2D(pair.left, pair.right)) {
        result.right = result.left;
        if (result.left_metric) {
          auto mirrored_metric = icp_metrics_[*result.left_metric];
          mirrored_metric.component = "right";
          icp_metrics_.push_back(std::move(mirrored_metric));
          result.right_metric = icp_metrics_.size() - 1;
        }
        return result;
      }
      if (!success_guide) {
        if (!result.left.success) return result;
        auto right = evaluate_side(pair.id, "right", pair.right, *right_cloud_, side_initial_pose,
                                   side_initial_pose);
        result.right = right.first;
        result.right_metric = right.second;
        return result;
      }

      if (result.left.success) {
        auto right = evaluate_side(pair.id, "right", pair.right, *right_cloud_, side_initial_pose,
                                   side_initial_pose);
        result.right = right.first;
        result.right_metric = right.second;
        if (!result.right.success) {
          retry_side_with_transform(pair.id, "right", pair.right, *right_cloud_,
                                    result.left.transform, side_initial_pose, &result.right,
                                    &result.right_metric);
        }
        return result;
      }

      auto right = evaluate_side(pair.id, "right", pair.right, *right_cloud_,
                                 Eigen::Matrix3f::Identity(), side_initial_pose);
      result.right = right.first;
      result.right_metric = right.second;
      if (!result.right.success) return result;
      retry_side_with_transform(pair.id, "left", pair.left, *left_cloud_, result.right.transform,
                                side_initial_pose, &result.left, &result.left_metric);
      return result;
    };

    const std::filesystem::path template_root(save_template_path_);
    std::vector<TemplatePair> pairs;
    std::vector<TemplatePairOutcome> outcomes;
    if (configuration_.template_candidates.empty()) {
      throw std::runtime_error("no template configured");
    }
    for (const auto &candidate : configuration_.template_candidates) {
      if (!std::filesystem::is_regular_file(template_root / candidate.left) ||
          !std::filesystem::is_regular_file(template_root / candidate.right)) {
        setStage(gap::core::FailureStage::kTemplateLoading, candidate.id);
        throw std::runtime_error("template missing or empty for candidate: " + candidate.id);
      }
    }
    std::size_t configuration_order = 0;
    for (const auto &candidate : configuration_.template_candidates) {
      setStage(gap::core::FailureStage::kTemplateLoading, candidate.id);
      pairs.push_back(load_template_pair(candidate.id, template_root / candidate.left,
                                         template_root / candidate.right, candidate.rois,
                                         configuration_order++));
      setStage(gap::core::FailureStage::kIcp, candidate.id);
      outcomes.push_back(evaluate_pair(pairs.back()));
    }

    std::vector<utils::TemplateIcpScore> scores;
    scores.reserve(outcomes.size());
    for (const auto &outcome : outcomes) {
      scores.push_back(
          {outcome.id, outcome.left.score, outcome.right.score, outcome.configuration_order});
    }
    const auto selected =
        utils::selectBestTemplateByIcp(scores, configuration_.alignment.icp.minimum_score);

    const auto mark_selected = [&](const TemplatePairOutcome &outcome) {
      if (outcome.left_metric) icp_metrics_[*outcome.left_metric].selected = true;
      if (outcome.right_metric) icp_metrics_[*outcome.right_metric].selected = true;
    };
    if (!selected) {
      mark_selected(outcomes.front());
      double failure_score = outcomes.front().left.score;
      if (outcomes.front().right_metric && std::isfinite(outcomes.front().right.score)) {
        failure_score = std::min(failure_score, outcomes.front().right.score);
      }
      std::ostringstream message;
      message << "ICP Failed: score=" << std::fixed << std::setprecision(2) << failure_score
              << ", minimum_score=" << configuration_.alignment.icp.minimum_score;
      throw std::runtime_error(message.str());
    }

    auto &selected_outcome = outcomes[*selected];
    configure_business_rois(selected_outcome.rois);
    // A candidate without rois only replays the config values, so the source stays "config".
    quality_.roi_source = selected_outcome.rois ? "template" : "config";

    // Layer 3: cross-side consistency gate. Delta = t_left^-1 * t_right; both sides register
    // template -> cloud with left/right templates cropped from one shared captured template
    // cloud (see the label-mode save above), so the nominal Delta is identity.
    const auto &consistency = robustness.consistency;
    quality_.consistency_mode = consistency.mode == domain::ConsistencyMode::kOff       ? "off"
                                : consistency.mode == domain::ConsistencyMode::kEnforce ? "enforce"
                                                                                        : "shadow";
    if (consistency.mode != domain::ConsistencyMode::kOff) {
      const auto compute_deviation = [&](const Eigen::Matrix3f &left_t,
                                         const Eigen::Matrix3f &right_t) {
        const Eigen::Matrix3f delta = left_t.inverse() * right_t;
        const double translation_mm = static_cast<double>(delta.block<2, 1>(0, 2).norm()) * scale_;
        const double rotation_deg =
            static_cast<double>(std::atan2(delta(1, 0), delta(0, 0))) * 180.0 / M_PI;
        return std::make_pair(translation_mm, rotation_deg);
      };

      auto [translation_mm, rotation_deg] =
          compute_deviation(selected_outcome.left.transform, selected_outcome.right.transform);
      quality_.consistency_translation_delta_mm = translation_mm;
      quality_.consistency_rotation_delta_deg = rotation_deg;
      const bool exceeds = translation_mm > consistency.max_translation_mm ||
                           std::fabs(rotation_deg) > consistency.max_rotation_deg;
      quality_.point_counts["consistency_gate_exceeded"] = exceeds ? 1 : 0;

      if (exceeds && consistency.mode == domain::ConsistencyMode::kEnforce) {
        quality_.consistency_retry_attempted = true;
        const bool left_is_worse = selected_outcome.left.score < selected_outcome.right.score;
        if (left_is_worse) {
          retry_side_with_transform(selected_outcome.id, "left", pairs[*selected].left,
                                    *left_cloud_, selected_outcome.right.transform,
                                    selected_outcome.trust_region_reference, &selected_outcome.left,
                                    &selected_outcome.left_metric);
        } else {
          retry_side_with_transform(selected_outcome.id, "right", pairs[*selected].right,
                                    *right_cloud_, selected_outcome.left.transform,
                                    selected_outcome.trust_region_reference,
                                    &selected_outcome.right, &selected_outcome.right_metric);
        }
        const auto retried_deviation =
            compute_deviation(selected_outcome.left.transform, selected_outcome.right.transform);
        translation_mm = retried_deviation.first;
        rotation_deg = retried_deviation.second;
        quality_.consistency_translation_delta_mm = translation_mm;
        quality_.consistency_rotation_delta_deg = rotation_deg;
        const bool still_exceeds = translation_mm > consistency.max_translation_mm ||
                                   std::fabs(rotation_deg) > consistency.max_rotation_deg;
        if (still_exceeds || !selected_outcome.left.success || !selected_outcome.right.success) {
          quality_.consistency_gate_failed = true;
          std::ostringstream message;
          message << "ICP Cross-Side Consistency Check Failed: translation_delta_mm=" << std::fixed
                  << std::setprecision(3) << translation_mm
                  << " (max=" << consistency.max_translation_mm
                  << "), rotation_delta_deg=" << rotation_deg
                  << " (max=" << consistency.max_rotation_deg << ")";
          throw std::runtime_error(message.str());
        }
        quality_.point_counts["consistency_retry_succeeded"] = 1;
      }
    }

    mark_selected(selected_outcome);
    const Eigen::Isometry2f t_left(selected_outcome.left.transform);
    const Eigen::Isometry2f t_right(selected_outcome.right.transform);
    if (configuration_.alignment.save_result) {
      saveAlignmentResult(*left_cloud_, selected_outcome.left.transform);
      saveAlignmentResult(*right_cloud_, selected_outcome.right.transform);
    }

    // first two roi for left cloud, last two roi for right cloud
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 2; j++) {
        rois_[i].col(j) = (i < 2 ? t_left : t_right) * rois_[i].col(j);
      }
    }
    compensation_centers_[0] = t_left * compensation_centers_[0];
    compensation_centers_[1] = t_right * compensation_centers_[1];
  }

  // Batch-run per-sample override: absolute sensor millimetres, applied on the sample's own
  // frame directly -- deliberately after the ICP transform block above, so it is never carried
  // through an alignment transform, and applied whether or not align_cloud_ took the ICP branch.
  if (configuration_.roi_override) {
    const auto &override_rois = *configuration_.roi_override;
    apply_business_rois({override_rois.flush_base.values, override_rois.gap_left.values,
                         override_rois.flush_ref.values, override_rois.gap_right.values});
    quality_.roi_source = configuration_.roi_override_source;
  }

  // Record the business ROIs actually used (after auto-centering, template override and ICP
  // alignment) for ML export/evaluation. Millimetres, [x_min, y_min, x_max, y_max].
  quality_.effective_roi["overall"] = resolved_overall_roi_mm_;
  static constexpr std::array<const char *, 4> kEffectiveRoiNames{"flush_base", "gap_left",
                                                                  "flush_ref", "gap_right"};
  for (int i = 0; i < 4; i++) {
    const Eigen::Vector2f min_pt = rois_[i].col(0) * static_cast<float>(scale_);
    const Eigen::Vector2f max_pt = rois_[i].col(1) * static_cast<float>(scale_);
    quality_.effective_roi[kEffectiveRoiNames[i]] = {min_pt.x(), min_pt.y(), max_pt.x(),
                                                     max_pt.y()};
  }

  // Intensity gate: drops points that sit inside a business ROI but are clearly darker than that
  // ROI's own median return level -- the ghost lines a secondary specular reflection leaves
  // behind, which radius-outlier removal cannot touch. Only clouds that really went through the
  // business-ROI crop are gated: the whole-cloud copy path carries the entire profile, whose
  // median (~19) says nothing about a surface (~120), and a no-filter type never produces a
  // cropped cloud in the first place. Called after the base_side swap, so the apply switches are
  // read by semantic role (base/ref) rather than by physical side.
  const auto &intensity_gate = configuration_.common.intensity_gate;
  const auto apply_intensity_gate = [&](const std::string &roi_name, const std::string &roi_type,
                                        bool apply, PointCloud *cloud) {
    if (!intensity_gate.enabled || !apply) return;
    if (!filter_by_business_roi || no_filter_types_.count(roi_type) != 0) return;
    const auto outcome = utils::filterCloudByIntensity(*cloud, cloud, intensity_gate);
    quality_.point_counts[roi_name + "_intensity_dropped"] =
        outcome.input_points - outcome.kept_points;
    quality_.point_counts[roi_name + "_intensity_blocked"] = outcome.applied ? 0 : 1;
  };

  // use right part of left camera and left part of right camera to compute gap and flush;
  double gap = std::nan("NaN"), flush = std::nan("NaN");
  if (flush_enable) {
    try {
      setStage(gap::core::FailureStage::kRoi, "flush");
      PointCloud::Ptr flush_base_cloud(new PointCloud);
      PointCloud::Ptr flush_ref_cloud(new PointCloud);
      std::string flush_left_type = flush_base_type, flush_right_type = flush_ref_type;
      if (flush_base == "right") std::swap(flush_left_type, flush_right_type);
      if (filter_by_business_roi && no_filter_types_.count(flush_left_type) == 0)
        CHECK_(gap_std::roiCrop2D(*left_cloud_, flush_base_cloud.get(), rois_[0]),
               "Left ROI of flush has no points");
      if (filter_by_business_roi && no_filter_types_.count(flush_right_type) == 0)
        CHECK_(gap_std::roiCrop2D(*right_cloud_, flush_ref_cloud.get(), rois_[2]),
               "Right ROI of flush has no points");
      if (!filter_by_business_roi) {
        pcl::copyPointCloud(*left_cloud_, *flush_base_cloud);
        pcl::copyPointCloud(*right_cloud_, *flush_ref_cloud);
      }
      if (flush_base == "right") std::swap(flush_base_cloud, flush_ref_cloud);
      apply_intensity_gate("flush_base", flush_base_type, intensity_gate.apply_flush_base,
                           flush_base_cloud.get());
      apply_intensity_gate("flush_ref", flush_ref_type, intensity_gate.apply_flush_ref,
                           flush_ref_cloud.get());
      quality_.point_counts["flush_base_roi"] = flush_base_cloud->size();
      quality_.point_counts["flush_ref_roi"] = flush_ref_cloud->size();
      setStage(gap::core::FailureStage::kFitting, "flush");
      flush = detectFlush(*flush_base_cloud, *flush_ref_cloud);
    } catch (const std::exception &error) {
      flush_failure_ = makeFailure("flush", error);
      if (!diagnostic_mode_) throw;
    }
  }

  if (gap_enable) {
    try {
      setStage(gap::core::FailureStage::kRoi, "gap");
      PointCloud::Ptr gap_left_cloud(new PointCloud);
      PointCloud::Ptr gap_right_cloud(new PointCloud);
      if (filter_by_business_roi && no_filter_types_.count(gap_left_type) == 0)
        CHECK_(gap_std::roiCrop2D(*left_cloud_, gap_left_cloud.get(), rois_[1]),
               "Left ROI of gap has no points");
      if (filter_by_business_roi && no_filter_types_.count(gap_right_type) == 0)
        CHECK_(gap_std::roiCrop2D(*right_cloud_, gap_right_cloud.get(), rois_[3]),
               "Right ROI of gap has no points");
      if (!filter_by_business_roi) {
        pcl::copyPointCloud(*left_cloud_, *gap_left_cloud);
        pcl::copyPointCloud(*right_cloud_, *gap_right_cloud);
      }
      apply_intensity_gate("gap_left", gap_left_type, intensity_gate.apply_gap_left,
                           gap_left_cloud.get());
      apply_intensity_gate("gap_right", gap_right_type, intensity_gate.apply_gap_right,
                           gap_right_cloud.get());
      quality_.point_counts["gap_left_roi"] = gap_left_cloud->size();
      quality_.point_counts["gap_right_roi"] = gap_right_cloud->size();
      setStage(gap::core::FailureStage::kFitting, "gap");
      gap = detectGap(*gap_left_cloud, *gap_right_cloud);
    } catch (const std::exception &error) {
      gap_failure_ = makeFailure("gap", error);
      if (!diagnostic_mode_) throw;
    }
  }
  quality_.icp = alignmentMetrics();
  auto end_time = std::chrono::system_clock::now();
  quality_.runtime_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
  setStage(gap::core::FailureStage::kNone);
  return quality_.runtime_us / 1000;
}

int64_t GapDetection::detect(const PointCloud &left_camera_cloud,
                             const PointCloud &right_camera_cloud) {
  return run(left_camera_cloud, right_camera_cloud);
}

std::string GapDetection::getFlush() const {
  return configuration_.flush.enabled ? formatTwoDecimals(flushMeasurementMm()) : "INACTIVE";
}

std::string GapDetection::getGap() const {
  if (gap_pts_.size() == 2 && gap_pts_[0][0] > gap_pts_[1][0]) {
    return "NAN";
  }
  return configuration_.gap.enabled ? formatTwoDecimals(gapMeasurementMm()) : "INACTIVE";
}

double GapDetection::flushMeasurementMm() const {
  double flush = getMeasurment(flush_pts_);
  if (flush_pts_.size() == 2) flush *= flush_pts_[0][1] > flush_pts_[1][1] ? 1 : -1;
  return std::fabs(flush) + configuration_.flush.offset;
}

double GapDetection::gapMeasurementMm() const {
  if (gap_pts_.size() == 2 && gap_pts_[0][0] > gap_pts_[1][0]) return std::nan("NaN");
  return std::fabs(getMeasurment(gap_pts_)) + configuration_.gap.offset;
}

std::string GapDetection::getRadius(bool is_left) const {
  const auto &type = is_left ? configuration_.gap.left_type : configuration_.gap.right_type;
  std::string circle = is_left ? "gap_left" : "gap_right";
  double r = circles_.find(circle) == circles_.end() ? std::nan("NAN") : circles_.at(circle)[2];
  return type == "circle" ? formatTwoDecimals(r * 1000) : "Disabled";
}

std::string GapDetection::getFlushNominal() const {
  return formatTwoDecimals(configuration_.flush.nominal);
}

std::string GapDetection::getGapNominal() const {
  return formatTwoDecimals(configuration_.gap.nominal);
}

std::string GapDetection::getLeftRadius() const { return getRadius(true); }

std::string GapDetection::getRightRadius() const { return getRadius(false); }

double GapDetection::getMeasurment(const std::vector<Eigen::Vector2f> &two_point) {
  if (two_point.size() != 2) return std::nan("NaN");
  Eigen::Vector2f dis = two_point[1] - two_point[0];
  return dis.norm() * 1000;
}

double GapDetection::detectFlush(const PointCloud &flush_base_cloud,
                                 const PointCloud &flush_ref_cloud) {
  const auto &flush_configuration = configuration_.flush;
  auto flush_base = flush_configuration.base_side;
  auto base_type = flush_configuration.base_type;
  auto ref_type = flush_configuration.reference_type;
  auto segment_points = flush_configuration.segment_points;
  auto line_fit_distance = configuration_.common.line_fit_distance / scale_;
  auto circle_fit_distance = configuration_.common.circle_fit_distance / scale_;

  Eigen::VectorXf base_line_eq, ref_line_eq;
  // get the geometry params for base and ref clouds
  for (int i = 0; i < 2; i++) {
    auto type = i == 0 ? base_type : ref_type;
    auto cloud = i == 0 ? flush_base_cloud : flush_ref_cloud;
    std::string side = i == 0 ? "base" : "ref";
    std::string side_name = i == 0 ? "base" : "ref";
    bool is_left_side = (i == 0) == (flush_base == "left");
    if (contains(type, "line")) {
      auto &line = i == 0 ? base_line_eq : ref_line_eq;
      if (type == "2-points line") {
        lines_["flush_" + side] = rois_[is_left_side ? 0 : 2];
        line = utils::getLinefrom2Points(lines_["flush_" + side].col(0),
                                         lines_["flush_" + side].col(1));
        continue;
      }

      pcl::Indices line_indices;
      CHECK_(gap_std::lineFit2D(cloud, &line, &line_indices, line_fit_distance),
             side_name + " flush line fit failed")

      if (segment_points < line_indices.size()) {
        bool ascend = cloud[0].x <= cloud.back().x;
        if (ascend == is_left_side) {
          line_indices.erase(line_indices.begin(), line_indices.end() - segment_points);
        } else {
          line_indices.resize(segment_points);
        }
        PointCloud line_cloud(cloud, line_indices);
        pcl::Indices second_line_indices;
        CHECK_(gap_std::lineFit2D(line_cloud, &line, &second_line_indices, line_fit_distance / 3),
               side_name + " flush line fit failed at second time")
        int j = 0;
        for (auto i : second_line_indices) line_indices[j++] = line_indices[i];
        line_indices.resize(second_line_indices.size());
      }
      recordLineQuality("flush_" + side, cloud, line, line_indices);

      if (align_cloud_) {
        lines_["flush_" + side] = utils::getIntersectionRL(rois_[is_left_side ? 0 : 2], line);
      } else {
        lines_["flush_" + side] = Eigen::Matrix2f::Identity();
        lines_["flush_" + side] << cloud[line_indices[0]].x, cloud[line_indices.back()].x,
            cloud[line_indices[0]].y, cloud[line_indices.back()].y;
      }

      // special case handeling
      if (type == "line end") {
        auto pt = utils::getEndPointofCloud(cloud, !is_left_side, &line_indices);
        flush_pts_.emplace_back(pt.x, pt.y);
      }
    } else if (contains(type, "circle")) {
      Eigen::VectorXf circle;
      pcl::Indices circle_indices;
      const bool circle_fitted =
          gap_std::circleFit2D(cloud, &circle, &circle_indices, circle_fit_distance);
      if (!circle_fitted && type == "circle end") {
        // A circle end is ultimately used as a point. Some glazing/reflection profiles only
        // preserve a short, nearly straight edge in this ROI, so retain the configured circle
        // result whenever it is available and fall back to a robust line endpoint only when the
        // circle model is impossible.
        Eigen::VectorXf fallback_line;
        pcl::Indices fallback_indices;
        CHECK_(gap_std::lineFit2D(cloud, &fallback_line, &fallback_indices, line_fit_distance),
               side_name + " flush circle fit failed and line endpoint fallback failed")
        recordLineQuality("flush_" + side + "_circle_end_fallback", cloud, fallback_line,
                          fallback_indices);
        const auto pt = utils::getEndPointofCloud(cloud, !is_left_side, &fallback_indices);
        flush_pts_.emplace_back(pt.x, pt.y);
        continue;
      }
      CHECK_(circle_fitted, side_name + " flush circle fit failed")
      // circles_.emplace_back(circle);
      circles_["flush_" + side] = circle;
      recordCircleQuality("flush_" + side, cloud, circle, circle_indices);

      // special case handeling
      if (type == "circle end") {
        auto pt = utils::getEndPointofCloud(cloud, !is_left_side, &circle_indices);
        flush_pts_.emplace_back(pt.x, pt.y);
      }
    } else if (type == "selected point") {
      auto origin_cloud = is_left_side ? left_cloud_ : right_cloud_;
      Eigen::Vector2f select_pt = rois_[is_left_side ? 0 : 2].col(0);
      Eigen::Vector2f near_to_select_pt;
      utils::pointCloudDistance(*origin_cloud, select_pt, &near_to_select_pt);
      flush_pts_.emplace_back(near_to_select_pt);
    }
  }

  // compute flush according to types
  base_type = type_map_[base_type];
  ref_type = type_map_[ref_type];
  double flush;
#define COMPUTE_FLUSH(l_type, r_type, distance_function) \
  if (base_type == (l_type) && ref_type == (r_type)) {   \
    flush = (distance_function);                         \
    break;                                               \
  }

  Eigen::Vector2f start, end;
  if (!flush_pts_.empty()) end = flush_pts_[0];
  do {
    COMPUTE_FLUSH("line", "point", utils::pointLineDistance(base_line_eq, end, &start))
    COMPUTE_FLUSH("line", "cloud",
                  utils::lineCloudDistance(base_line_eq, flush_ref_cloud, &start, &end))
    COMPUTE_FLUSH("circle", "point",
                  utils::pointCircleDistance(circles_["flush_base"], end, &start))
    COMPUTE_FLUSH("circle", "cloud",
                  utils::circleCloudDistance(circles_["flush_base"], flush_ref_cloud, &start, &end))
    CHECK_(false, "Flush: Not Implemented Types");
  } while (false);

  if (flush_pts_.size() < 2) {
    flush_pts_.resize(2);
    flush_pts_[0] = start;
    flush_pts_[1] = end;

    // extend segment for vis, only for now situation
    std::string line = "flush_base";
    if (lines_.find(line) != lines_.end())
      lines_[line] = utils::insertPoint2Segment(lines_[line], start);
  }
  return flush;
}

double GapDetection::detectGap(const PointCloud &gap_left_cloud,
                               const PointCloud &gap_right_cloud) {
  const auto &gap_configuration = configuration_.gap;
  auto gap_left_type = gap_configuration.left_type;
  auto gap_right_type = gap_configuration.right_type;
  auto segment_points = gap_configuration.segment_points;

  const bool gap_definition_a = gap_configuration.definition == "A";
  if (gap_definition_a) {
    CHECK_(gap_left_type == "circle" && gap_right_type == "circle",
           "Gap definition A requires circle type on both sides")
    CHECK_(configuration_.flush.enabled && lines_.count("flush_base") == 1,
           "Gap definition A requires the flush base line")
  }

  auto is_fixed_radius_left = gap_configuration.left_radius.fixed;
  auto left_circle_radius_min = gap_configuration.left_radius.minimum / scale_;
  auto left_circle_radius_max = gap_configuration.left_radius.maximum / scale_;
  auto left_circle_radius_fixed = gap_configuration.left_radius.value / scale_;
  auto is_fixed_radius_right = gap_configuration.right_radius.fixed;
  auto right_circle_radius_min = gap_configuration.right_radius.minimum / scale_;
  auto right_circle_radius_max = gap_configuration.right_radius.maximum / scale_;
  auto right_circle_radius_fixed = gap_configuration.right_radius.value / scale_;
  auto line_fit_distance = configuration_.common.line_fit_distance / scale_;
  auto circle_fit_distance = configuration_.common.circle_fit_distance / scale_;

  struct DeferredCameraCircles {
    bool active = false;
    std::array<PointCloud, 2> clouds;
    std::array<Eigen::VectorXf, 2> circles;
    std::array<pcl::Indices, 2> indices;
  };
  std::array<DeferredCameraCircles, 2> deferred_camera_circles;

  // get the geometry params for left and right clouds
  Eigen::VectorXf left_line_eq, right_line_eq;
  for (int i = 0; i < 2; i++) {
    auto type = i == 0 ? gap_left_type : gap_right_type;
    auto cloud = i == 0 ? gap_left_cloud : gap_right_cloud;
    auto &line = i == 0 ? left_line_eq : right_line_eq;
    std::string side = i == 0 ? "left" : "right";
    std::string side_name = i == 0 ? "left" : "right";
    if (contains(type, "line")) {
      pcl::Indices line_indices;
      CHECK_(gap_std::lineFit2D(cloud, &line, &line_indices, type, line_fit_distance),
             side_name + " gap line fit failed")
      if (segment_points < line_indices.size()) {
        bool ascend = cloud[0].x <= cloud.back().x;
        if (ascend == (i == 0)) {
          line_indices.erase(line_indices.begin(), line_indices.end() - segment_points);
        } else {
          line_indices.resize(segment_points);
        }
        PointCloud line_cloud(cloud, line_indices);
        pcl::Indices second_line_indices;
        CHECK_(gap_std::lineFit2D(line_cloud, &line, &second_line_indices, line_fit_distance / 3),
               side_name + " gap line fit failed at second time")
        int j = 0;
        for (auto i : second_line_indices) line_indices[j++] = line_indices[i];
        line_indices.resize(second_line_indices.size());
      }
      recordLineQuality("gap_" + side, cloud, line, line_indices);

      if (align_cloud_) {
        lines_["gap_" + side] = utils::getIntersectionRL(rois_[2 * i + 1], line);
      } else {
        lines_["gap_" + side] = Eigen::Matrix2f::Identity();
        lines_["gap_" + side] << cloud[line_indices[0]].x, cloud[line_indices.back()].x,
            cloud[line_indices[0]].y, cloud[line_indices.back()].y;
      }

      // special case handeling
    } else if (contains(type, "circle")) {
      Eigen::VectorXf circle;
      pcl::Indices circle_indices;
      const auto &compensation =
          i == 0 ? gap_configuration.left_compensation : gap_configuration.right_compensation;
      bool is_fixed_radius = i == 0 ? is_fixed_radius_left : is_fixed_radius_right;
      double circle_radius_min = i == 0 ? left_circle_radius_min : right_circle_radius_min;
      double circle_radius_max = i == 0 ? left_circle_radius_max : right_circle_radius_max;
      double radius_fixed = i == 0 ? left_circle_radius_fixed : right_circle_radius_fixed;
      if (!is_fixed_radius || radius_fixed <= 0) {
        radius_fixed = 0;
      } else {
        CHECK_(circle_radius_max > radius_fixed, "max radius should be lager than fixed radius")
        CHECK_(circle_radius_min < radius_fixed, "min radius should be smaller than fixed radius")
      }
      CHECK_(circle_radius_max > circle_radius_min, "max radius should be lager than min radius")
      std::string circle_model = "circle";
      // Tracks the fixed-radius value actually used by whichever circleFit2D call below produces
      // the final `circle`, for radius_mode classification once the branch is resolved -- the
      // camera-separated fallback can override this to 0 (a free fit) even when this side's
      // config requests a fixed radius (select_closest_nominal).
      double effective_fixed_radius = radius_fixed;
      if (compensation.enabled) {
        utils::FixedRadiusCircleOptions options;
        options.radius = radius_fixed;
        options.nominal_center = compensation_centers_[i];
        options.maximum_center_shift = compensation.maximum_center_shift / scale_;
        options.maximum_point_gap = compensation.maximum_point_gap / scale_;
        options.inlier_distance = compensation.inlier_distance / scale_;
        options.minimum_inliers = compensation.minimum_inliers;
        options.minimum_arc_coverage_deg = compensation.minimum_arc_coverage_deg;
        options.maximum_rms_residual = compensation.maximum_rms_residual / scale_;

        PointCloud primary_roi;
        PointCloud secondary_roi;
        gap_std::roiCrop2D(*left_camera_cloud_, &primary_roi, rois_[2 * i + 1]);
        gap_std::roiCrop2D(*right_camera_cloud_, &secondary_roi, rois_[2 * i + 1]);
        const auto quality_prefix = "gap_" + side + "_compensation_";
        quality_.point_counts[quality_prefix + "primary_roi"] = primary_roi.size();
        quality_.point_counts[quality_prefix + "secondary_roi"] = secondary_roi.size();

        utils::FixedRadiusCircleResult primary_result;
        utils::FixedRadiusCircleResult secondary_result;
        const bool try_primary = compensation.preferred_camera != "Right";
        const bool try_secondary = compensation.preferred_camera != "Left";
        const bool primary_success = try_primary && utils::fitFixedRadiusCircleComponents(
                                                        primary_roi, options, &primary_result);
        const bool secondary_success =
            try_secondary &&
            utils::fitFixedRadiusCircleComponents(secondary_roi, options, &secondary_result);
        // Losing candidates are recorded for evaluation only. They stay behind the export
        // switch because minimumInlierRatio()/maximumFitResidualMm() reduce over every
        // fits[] entry, and those two values reach results.csv and the measurement
        // database -- an extra entry would silently move them in normal production runs.
        if (export_fit_points_ && primary_success) {
          recordCircleQuality(quality_prefix + "primary_candidate", primary_roi,
                              primary_result.circle, primary_result.inliers, "fixed-radius-circle",
                              "fixed");
        }
        if (export_fit_points_ && secondary_success) {
          recordCircleQuality(quality_prefix + "secondary_candidate", secondary_roi,
                              secondary_result.circle, secondary_result.inliers,
                              "fixed-radius-circle", "fixed");
        }
        const bool prefer_primary =
            primary_success &&
            (!secondary_success ||
             primary_result.inliers.size() > secondary_result.inliers.size() ||
             (primary_result.inliers.size() == secondary_result.inliers.size() &&
              (primary_result.arc_coverage_deg > secondary_result.arc_coverage_deg + 1e-9 ||
               (std::fabs(primary_result.arc_coverage_deg - secondary_result.arc_coverage_deg) <=
                    1e-9 &&
                primary_result.rms_residual <= secondary_result.rms_residual))));

        if (prefer_primary) {
          cloud = std::move(primary_roi);
          circle = std::move(primary_result.circle);
          circle_indices = std::move(primary_result.inliers);
        } else if (secondary_success) {
          cloud = std::move(secondary_roi);
          circle = std::move(secondary_result.circle);
          circle_indices = std::move(secondary_result.inliers);
        } else {
          CHECK_(compensation.fallback_to_nominal, side_name + " compensated circle fit failed")
          cloud = compensation.preferred_camera == "Right" ? std::move(secondary_roi)
                                                           : std::move(primary_roi);
          circle = Eigen::VectorXf(3);
          circle << options.nominal_center.x(), options.nominal_center.y(), options.radius;
          circle_model = "nominal-fixed-circle";
        }
        if (!circle_indices.empty()) circle_model = "fixed-radius-circle";
        quality_.point_counts[quality_prefix + "inliers"] = circle_indices.size();
        quality_.point_counts[quality_prefix + "fallback"] = circle_indices.empty() ? 1 : 0;
      } else {
        bool circle_fitted = gap_std::circleFit2D(cloud, &circle, &circle_indices, circle_fit_distance,
                                              circle_radius_min, circle_radius_max, radius_fixed);
        if (!circle_fitted && gap_configuration.circle_fit_retry_distance > 0) {
          circle_indices.clear();
          const auto retry_distance = gap_configuration.circle_fit_retry_distance / scale_;
          circle_fitted = gap_std::circleFit2D(cloud, &circle, &circle_indices, retry_distance,
                                           circle_radius_min, circle_radius_max, radius_fixed);
          const auto quality_prefix = "gap_" + side + "_circle_retry_";
          quality_.point_counts[quality_prefix + "attempted"] = 1;
          quality_.point_counts[quality_prefix + "distance_um"] = static_cast<std::size_t>(
              std::llround(gap_configuration.circle_fit_retry_distance * 1000.0));
          quality_.point_counts[quality_prefix + "inliers"] = circle_indices.size();
          quality_.point_counts[quality_prefix + "success"] = circle_fitted ? 1 : 0;
          if (circle_fitted) circle_model = "circle-retry";
        }
        if (!circle_fitted && gap_configuration.camera_separated_circle_fallback) {
          PointCloud primary_roi;
          PointCloud secondary_roi;
          gap_std::roiCrop2D(*left_camera_cloud_, &primary_roi, rois_[2 * i + 1]);
          gap_std::roiCrop2D(*right_camera_cloud_, &secondary_roi, rois_[2 * i + 1]);
          const auto quality_prefix = "gap_" + side + "_camera_fallback_";
          quality_.point_counts[quality_prefix + "primary_roi"] = primary_roi.size();
          quality_.point_counts[quality_prefix + "secondary_roi"] = secondary_roi.size();

          Eigen::VectorXf primary_circle;
          Eigen::VectorXf secondary_circle;
          pcl::Indices primary_indices;
          pcl::Indices secondary_indices;
          const auto &preferred_camera = gap_configuration.camera_separated_preferred_camera;
          const bool select_closest_nominal =
              gap_configuration.camera_separated_select_closest_nominal;
          const double separated_radius_fixed = select_closest_nominal ? 0.0 : radius_fixed;
          effective_fixed_radius = separated_radius_fixed;
          const bool primary_success =
              gap_std::circleFit2D(primary_roi, &primary_circle, &primary_indices, circle_fit_distance,
                               circle_radius_min, circle_radius_max, separated_radius_fixed);
          const bool secondary_success = gap_std::circleFit2D(
              secondary_roi, &secondary_circle, &secondary_indices, circle_fit_distance,
              circle_radius_min, circle_radius_max, separated_radius_fixed);
          quality_.point_counts[quality_prefix + "primary_inliers"] = primary_indices.size();
          quality_.point_counts[quality_prefix + "secondary_inliers"] = secondary_indices.size();
          if (primary_success) {
            recordCircleQuality(
                "gap_" + side + "_camera_primary_candidate", primary_roi, primary_circle,
                primary_indices, "camera-separated-circle-candidate",
                utils::classifyRadiusMode(primary_circle[2], circle_radius_min, circle_radius_max,
                                          separated_radius_fixed > 0));
          }
          if (secondary_success) {
            recordCircleQuality(
                "gap_" + side + "_camera_secondary_candidate", secondary_roi, secondary_circle,
                secondary_indices, "camera-separated-circle-candidate",
                utils::classifyRadiusMode(secondary_circle[2], circle_radius_min, circle_radius_max,
                                          separated_radius_fixed > 0));
          }

          constexpr std::size_t kMinimumCameraInliers = 8;
          const double maximum_radius_difference = 0.25 / scale_;
          const double maximum_center_difference = 0.75 / scale_;
          if (primary_success && secondary_success) {
            const auto radius_delta = std::fabs(primary_circle[2] - secondary_circle[2]);
            const auto center_delta =
                (primary_circle.head<2>() - secondary_circle.head<2>()).norm();
            quality_.point_counts[quality_prefix + "radius_delta_um"] =
                static_cast<std::size_t>(std::llround(radius_delta * scale_ * 1000.0));
            quality_.point_counts[quality_prefix + "center_delta_um"] =
                static_cast<std::size_t>(std::llround(center_delta * scale_ * 1000.0));
          }
          const bool consistent =
              primary_success && secondary_success &&
              primary_indices.size() >= kMinimumCameraInliers &&
              secondary_indices.size() >= kMinimumCameraInliers &&
              std::fabs(primary_circle[2] - secondary_circle[2]) <= maximum_radius_difference &&
              (primary_circle.head<2>() - secondary_circle.head<2>()).norm() <=
                  maximum_center_difference;
          quality_.point_counts[quality_prefix + "consistent"] = consistent ? 1 : 0;
          const bool primary_eligible =
              primary_success && primary_indices.size() >= kMinimumCameraInliers;
          const bool secondary_eligible =
              secondary_success && secondary_indices.size() >= kMinimumCameraInliers;
          quality_.point_counts[quality_prefix + "preferred_primary"] =
              preferred_camera == "Left" ? 1 : 0;
          quality_.point_counts[quality_prefix + "preferred_secondary"] =
              preferred_camera == "Right" ? 1 : 0;
          const bool can_select_by_gap_nominal =
              select_closest_nominal && gap_left_type == "circle" && gap_right_type == "circle";
          if (can_select_by_gap_nominal && primary_eligible && secondary_eligible) {
            auto &deferred = deferred_camera_circles[static_cast<std::size_t>(i)];
            deferred.active = true;
            deferred.clouds[0] = primary_roi;
            deferred.clouds[1] = secondary_roi;
            deferred.circles[0] = primary_circle;
            deferred.circles[1] = secondary_circle;
            deferred.indices[0] = primary_indices;
            deferred.indices[1] = secondary_indices;
            cloud = primary_roi;
            circle = primary_circle;
            circle_indices = primary_indices;
            circle_model = "camera-separated-circle-pending-nominal-selection";
            circle_fitted = true;
            quality_.point_counts[quality_prefix + "nominal_selection_pending"] = 1;
          } else if (select_closest_nominal && primary_eligible && !secondary_eligible) {
            cloud = std::move(primary_roi);
            circle = std::move(primary_circle);
            circle_indices = std::move(primary_indices);
            circle_model = "camera-separated-circle-primary-only-eligible";
            circle_fitted = true;
            quality_.point_counts[quality_prefix + "only_eligible_selected"] = 1;
          } else if (select_closest_nominal && secondary_eligible && !primary_eligible) {
            cloud = std::move(secondary_roi);
            circle = std::move(secondary_circle);
            circle_indices = std::move(secondary_indices);
            circle_model = "camera-separated-circle-secondary-only-eligible";
            circle_fitted = true;
            quality_.point_counts[quality_prefix + "only_eligible_selected"] = 2;
          } else if (preferred_camera == "Left" && primary_eligible) {
            cloud = std::move(primary_roi);
            circle = std::move(primary_circle);
            circle_indices = std::move(primary_indices);
            circle_model = "camera-separated-circle-primary-preferred";
            circle_fitted = true;
            quality_.point_counts[quality_prefix + "preferred_selected"] = 1;
          } else if (preferred_camera == "Right" && secondary_eligible) {
            cloud = std::move(secondary_roi);
            circle = std::move(secondary_circle);
            circle_indices = std::move(secondary_indices);
            circle_model = "camera-separated-circle-secondary-preferred";
            circle_fitted = true;
            quality_.point_counts[quality_prefix + "preferred_selected"] = 2;
          } else if (consistent && primary_indices.size() >= secondary_indices.size()) {
            cloud = std::move(primary_roi);
            circle = std::move(primary_circle);
            circle_indices = std::move(primary_indices);
            circle_model = "camera-separated-circle-primary";
            circle_fitted = true;
          } else if (consistent) {
            cloud = std::move(secondary_roi);
            circle = std::move(secondary_circle);
            circle_indices = std::move(secondary_indices);
            circle_model = "camera-separated-circle-secondary";
            circle_fitted = true;
          }
        }
        CHECK_(circle_fitted, side_name + " gap circle fit failed")
      }
      circles_["gap_" + side] = circle;
      std::string radius_mode;
      if (circle.size() >= 3) {
        radius_mode =
            compensation.enabled
                ? (circle_model == "nominal-fixed-circle" ? "nominal" : "fixed")
                : utils::classifyRadiusMode(circle[2], circle_radius_min, circle_radius_max,
                                            effective_fixed_radius > 0);
      }
      if (!deferred_camera_circles[static_cast<std::size_t>(i)].active) {
        recordCircleQuality("gap_" + side, cloud, circle, circle_indices, circle_model,
                            radius_mode);
      }

      // special case handeling
      if (type == "circle tangent") {
        auto pt = utils::getEndPointofCloud(cloud, i != 0, &circle_indices);
        Eigen::Vector2f tangent_point;
        utils::pointCircleDistance(circle, {pt.x, pt.y}, &tangent_point);
        lines_["gap_" + side].col(0) = tangent_point;
        lines_["gap_" + side].col(1) = utils::rotatePoint(circle.head<2>(), tangent_point, 90);
        line =
            utils::getLinefrom2Points(lines_["gap_" + side].col(0), lines_["gap_" + side].col(1));
      }
    } else if (type == "selected point") {
      // input gap cloud has been filter to empty, thus origin cloud is needed
      auto origin_cloud = i == 0 ? left_cloud_ : right_cloud_;
      Eigen::Vector2f select_pt = rois_[2 * i + 1].col(0);
      Eigen::Vector2f near_to_select_pt;
      utils::pointCloudDistance(*origin_cloud, select_pt, &near_to_select_pt);
      gap_pts_.emplace_back(near_to_select_pt);
    }
  }

  if (deferred_camera_circles[0].active || deferred_camera_circles[1].active) {
    const int left_candidate_count = deferred_camera_circles[0].active ? 2 : 1;
    const int right_candidate_count = deferred_camera_circles[1].active ? 2 : 1;
    double best_nominal_delta_mm = std::numeric_limits<double>::infinity();
    double best_gap_mm = std::numeric_limits<double>::quiet_NaN();
    std::size_t best_inlier_count = 0;
    int best_left_candidate = 0;
    int best_right_candidate = 0;
    bool found_valid_candidate = false;
    int candidate_number = 0;

    for (int left_candidate = 0; left_candidate < left_candidate_count; ++left_candidate) {
      if (deferred_camera_circles[0].active) {
        circles_["gap_left"] = deferred_camera_circles[0].circles[left_candidate];
      }
      for (int right_candidate = 0; right_candidate < right_candidate_count;
           ++right_candidate, ++candidate_number) {
        if (deferred_camera_circles[1].active) {
          circles_["gap_right"] = deferred_camera_circles[1].circles[right_candidate];
        }
        Eigen::Vector2f candidate_start;
        Eigen::Vector2f candidate_end;
        const auto distance = utils::circleCircleDistance(
            circles_["gap_left"], circles_["gap_right"], &candidate_start, &candidate_end);
        const auto candidate_gap_mm = std::fabs(distance * 1000.0) + gap_configuration.offset;
        const bool valid_candidate =
            std::isfinite(candidate_gap_mm) && candidate_start.x() <= candidate_end.x();
        const auto diagnostic_prefix =
            "gap_camera_nominal_candidate_" + std::to_string(candidate_number) + "_";
        quality_.point_counts[diagnostic_prefix + "valid"] = valid_candidate ? 1 : 0;
        if (!valid_candidate) continue;

        const auto nominal_delta_mm = std::fabs(candidate_gap_mm - gap_configuration.nominal);
        quality_.point_counts[diagnostic_prefix + "gap_um"] =
            static_cast<std::size_t>(std::llround(candidate_gap_mm * 1000.0));
        quality_.point_counts[diagnostic_prefix + "nominal_delta_um"] =
            static_cast<std::size_t>(std::llround(nominal_delta_mm * 1000.0));
        const auto inlier_count = (deferred_camera_circles[0].active
                                       ? deferred_camera_circles[0].indices[left_candidate].size()
                                       : 0) +
                                  (deferred_camera_circles[1].active
                                       ? deferred_camera_circles[1].indices[right_candidate].size()
                                       : 0);
        const bool better = nominal_delta_mm < best_nominal_delta_mm - 1e-9 ||
                            (std::fabs(nominal_delta_mm - best_nominal_delta_mm) <= 1e-9 &&
                             inlier_count > best_inlier_count);
        if (better) {
          found_valid_candidate = true;
          best_nominal_delta_mm = nominal_delta_mm;
          best_gap_mm = candidate_gap_mm;
          best_inlier_count = inlier_count;
          best_left_candidate = left_candidate;
          best_right_candidate = right_candidate;
        }
      }
    }

    CHECK_(found_valid_candidate, "gap nominal camera selection failed")
    const auto select_deferred_circle = [&](std::size_t side, int candidate,
                                            const std::string &component) {
      auto &deferred = deferred_camera_circles[side];
      if (!deferred.active) return;
      const auto camera_name = candidate == 0 ? "primary" : "secondary";
      const auto &selected_circle = deferred.circles[candidate];
      circles_[component] = selected_circle;
      // Only reached when camera_separated_select_closest_nominal gated this side into deferred
      // resolution, which always fits with a free (non-fixed) radius -- see effective_fixed_radius
      // above.
      const double min_radius = side == 0 ? left_circle_radius_min : right_circle_radius_min;
      const double max_radius = side == 0 ? left_circle_radius_max : right_circle_radius_max;
      const std::string radius_mode =
          selected_circle.size() >= 3
              ? utils::classifyRadiusMode(selected_circle[2], min_radius, max_radius, false)
              : std::string();
      recordCircleQuality(
          component, deferred.clouds[candidate], deferred.circles[candidate],
          deferred.indices[candidate],
          "camera-separated-circle-" + std::string(camera_name) + "-closest-gap-nominal",
          radius_mode);
      quality_.point_counts[component + "_camera_nominal_selected"] = candidate + 1;
    };
    select_deferred_circle(0, best_left_candidate, "gap_left");
    select_deferred_circle(1, best_right_candidate, "gap_right");
    quality_.point_counts["gap_camera_nominal_selected_gap_um"] =
        static_cast<std::size_t>(std::llround(best_gap_mm * 1000.0));
    quality_.point_counts["gap_camera_nominal_selected_delta_um"] =
        static_cast<std::size_t>(std::llround(best_nominal_delta_mm * 1000.0));
  }

  // compute gap according to types
  gap_left_type = type_map_[gap_left_type];
  gap_right_type = type_map_[gap_right_type];
  double gap;
#define COMPUTE_GAP(l_type, r_type, distance_function)           \
  if (gap_left_type == (l_type) && gap_right_type == (r_type)) { \
    gap = (distance_function);                                   \
    break;                                                       \
  }

  Eigen::Vector2f start, end;
  if (gap_left_type == "point") start = gap_pts_[0];
  if (gap_right_type == "point") end = gap_pts_[0];
  if (gap_pts_.size() == 2) {
    start = gap_pts_[0];
    end = gap_pts_[1];
  }
  do {
    if (gap_definition_a) {
      // Definition A: gap between the two tangent lines perpendicular to the flush datum
      // surface, instead of the closest distance along the circle-center line.
      Eigen::Vector2f u = (lines_["flush_base"].col(1) - lines_["flush_base"].col(0)).normalized();
      if (u.x() < 0) u = -u;  // point from left panel toward right panel
      const Eigen::Vector2f c1 = circles_["gap_left"].head<2>();
      const Eigen::Vector2f c2 = circles_["gap_right"].head<2>();
      const float r1 = circles_["gap_left"][2];
      const float r2 = circles_["gap_right"][2];
      const float gap_a = (c2 - c1).dot(u) - r1 - r2;
      CHECK_(gap_a >= 0, "Gap definition A: tangent lines overlap")
      start = c1 + r1 * u;      // tangent point on left circle
      end = start + gap_a * u;  // its foot on the right tangent line
      gap = gap_a;

      // Store the two tangent lines for visualization, same column-endpoint format as other
      // lines_ entries.
      const Eigen::Vector2f v(-u.y(), u.x());
      lines_["gap_tangent_left"].col(0) = start - r1 * 1.5f * v;
      lines_["gap_tangent_left"].col(1) = start + r1 * 1.5f * v;
      const Eigen::Vector2f t2 = c2 - r2 * u;
      lines_["gap_tangent_right"].col(0) = t2 - r2 * 1.5f * v;
      lines_["gap_tangent_right"].col(1) = t2 + r2 * 1.5f * v;
      break;
    }
    COMPUTE_GAP("point", "point", utils::pointPointDistance(start, end))
    COMPUTE_GAP("point", "line", utils::pointLineDistance(right_line_eq, start, &end))
    COMPUTE_GAP("line", "point", utils::pointLineDistance(left_line_eq, end, &start))
    COMPUTE_GAP("point", "circle", utils::pointCircleDistance(circles_["gap_right"], start, &end))
    COMPUTE_GAP("circle", "point", utils::pointCircleDistance(circles_["gap_left"], end, &start))
    COMPUTE_GAP("point", "cloud", utils::pointCloudDistance(gap_right_cloud, start, &end))
    COMPUTE_GAP("cloud", "point", utils::pointCloudDistance(gap_left_cloud, end, &start))
    COMPUTE_GAP("line", "cloud",
                utils::lineCloudDistance(left_line_eq, gap_right_cloud, &start, &end))
    COMPUTE_GAP("cloud", "line",
                utils::lineCloudDistance(right_line_eq, gap_left_cloud, &end, &start))
    COMPUTE_GAP("line", "circle",
                utils::lineCircleDistance(left_line_eq, circles_["gap_right"], &start, &end))
    COMPUTE_GAP("circle", "line",
                utils::lineCircleDistance(right_line_eq, circles_["gap_left"], &end, &start))
    COMPUTE_GAP("circle", "cloud",
                utils::circleCloudDistance(circles_["gap_left"], gap_right_cloud, &start, &end))
    COMPUTE_GAP("cloud", "circle",
                utils::circleCloudDistance(circles_["gap_right"], gap_left_cloud, &end, &start))
    COMPUTE_GAP("cloud", "cloud",
                utils::cloudCloudDistance(gap_left_cloud, gap_right_cloud, &start, &end))
    COMPUTE_GAP(
        "circle", "circle",
        utils::circleCircleDistance(circles_["gap_left"], circles_["gap_right"], &start, &end))
    CHECK_(false, "Gap: Not Implemented Types");
  } while (false);

  if (gap_pts_.size() < 2) {
    gap_pts_.resize(2);
    gap_pts_[0] = start;
    gap_pts_[1] = end;

    if (gap_left_type == "line" && lines_.find("gap_left") != lines_.end())
      lines_["gap_left"] = utils::insertPoint2Segment(lines_["gap_left"], start);
    if (gap_right_type == "line" && lines_.find("gap_right") != lines_.end())
      lines_["gap_right"] = utils::insertPoint2Segment(lines_["gap_right"], end);
  }
  return gap;
}

void GapDetection::fillMlExportFields(gap::core::FitQuality &metric, const PointCloud &cloud,
                                      const pcl::Indices &inliers) const {
  if (!export_fit_points_) return;
  metric.roi_points_mm.reserve(cloud.size());
  for (const auto &p : cloud) {
    metric.roi_points_mm.push_back(
        {static_cast<double>(p.x) * 1000.0, static_cast<double>(p.y) * 1000.0});
  }
  metric.inlier_mask.assign(cloud.size(), 0);
  for (auto idx : inliers) {
    if (idx >= 0 && static_cast<std::size_t>(idx) < metric.inlier_mask.size()) {
      metric.inlier_mask[idx] = 1;
    }
  }
}

void GapDetection::recordLineQuality(const std::string &component, const PointCloud &cloud,
                                     const Eigen::VectorXf &line, const pcl::Indices &inliers) {
  gap::core::FitQuality metric;
  metric.component = component;
  metric.model = "line";
  metric.point_count = cloud.size();
  metric.inlier_count = inliers.size();
  metric.inlier_ratio =
      cloud.empty() ? std::nan("NaN") : static_cast<double>(inliers.size()) / cloud.size();
  if (line.size() >= 5) {
    metric.line_point_x_mm = line[0] * 1000.0;
    metric.line_point_y_mm = line[1] * 1000.0;
    const double dir_norm = std::hypot(line[3], line[4]);
    if (dir_norm > 0) {
      metric.line_dir_x = line[3] / dir_norm;
      metric.line_dir_y = line[4] / dir_norm;
    }
  }
  fillMlExportFields(metric, cloud, inliers);
  if (line.size() >= 5 && !inliers.empty()) {
    const double dx = line[3];
    const double dy = line[4];
    const double norm = std::hypot(dx, dy);
    double square_sum = 0;
    double maximum = 0;
    std::size_t valid = 0;
    if (norm > 0) {
      for (const auto index : inliers) {
        if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
        const auto &point = cloud[index];
        const auto residual =
            std::fabs((point.x - line[0]) * dy - (point.y - line[1]) * dx) / norm * 1000;
        square_sum += residual * residual;
        maximum = std::max(maximum, residual);
        ++valid;
      }
    }
    if (valid > 0) {
      metric.rms_residual_mm = std::sqrt(square_sum / valid);
      metric.max_residual_mm = maximum;
    }
  }
  quality_.fits.push_back(std::move(metric));
}

void GapDetection::recordCircleQuality(const std::string &component, const PointCloud &cloud,
                                       const Eigen::VectorXf &circle, const pcl::Indices &inliers,
                                       std::string model, std::string radius_mode) {
  gap::core::FitQuality metric;
  metric.component = component;
  metric.model = std::move(model);
  metric.radius_mode = std::move(radius_mode);
  metric.point_count = cloud.size();
  metric.inlier_count = inliers.size();
  metric.inlier_ratio =
      cloud.empty() ? std::nan("NaN") : static_cast<double>(inliers.size()) / cloud.size();
  if (circle.size() >= 3) {
    metric.radius_mm = circle[2] * 1000;
    metric.center_x_mm = circle[0] * 1000.0;
    metric.center_y_mm = circle[1] * 1000.0;
  }
  fillMlExportFields(metric, cloud, inliers);
  if (circle.size() >= 3 && !inliers.empty()) {
    double square_sum = 0;
    double maximum = 0;
    std::vector<double> angles;
    angles.reserve(inliers.size());
    for (const auto index : inliers) {
      if (index < 0 || static_cast<std::size_t>(index) >= cloud.size()) continue;
      const auto &point = cloud[index];
      const double delta_x = point.x - circle[0];
      const double delta_y = point.y - circle[1];
      const double residual = std::fabs(std::hypot(delta_x, delta_y) - circle[2]) * 1000;
      square_sum += residual * residual;
      maximum = std::max(maximum, residual);
      auto angle = std::atan2(delta_y, delta_x);
      if (angle < 0) angle += 2 * M_PI;
      angles.push_back(angle);
    }
    if (!angles.empty()) {
      metric.rms_residual_mm = std::sqrt(square_sum / angles.size());
      metric.max_residual_mm = maximum;
      std::sort(angles.begin(), angles.end());
      double largest_gap = angles.front() + 2 * M_PI - angles.back();
      for (std::size_t i = 1; i < angles.size(); ++i) {
        largest_gap = std::max(largest_gap, angles[i] - angles[i - 1]);
      }
      metric.arc_coverage_deg = (2 * M_PI - largest_gap) * 180.0 / M_PI;
    }
  }
  quality_.fits.push_back(std::move(metric));
}

std::unordered_map<std::string, std::string> GapDetection::type_map_ = {
    {"selected point", "point"}, {"circle end", "point"},    {"circle", "circle"},
    {"circle tangent", "line"},  {"line", "line"},           {"horizontal line", "line"},
    {"vertical line", "line"},   {"line end", "point"},      {"fit line", "line"},
    {"2-points line", "line"},   {"nearest point", "cloud"},
};

std::unordered_set<std::string> GapDetection::no_filter_types_ = {"2-points line",
                                                                  "selected point"};
}  // namespace detection
