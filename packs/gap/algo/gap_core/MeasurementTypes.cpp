#include "gap_core/MeasurementTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace gap::core {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

template <typename Range, typename Projection, typename Comparator>
double finiteExtremum(const Range& values, Projection projection, Comparator comparator) {
  double result = std::numeric_limits<double>::quiet_NaN();
  for (const auto& value : values) {
    const auto projected = projection(value);
    if (!std::isfinite(projected)) continue;
    if (!std::isfinite(result) || comparator(projected, result)) result = projected;
  }
  return result;
}

std::string formatFiniteNumber(double value) {
  if (!std::isfinite(value)) return "NAN";
  std::ostringstream output;
  output << std::fixed << std::setprecision(2) << value;
  return output.str();
}

}  // namespace

double QualityMetrics::minimumInlierRatio() const {
  return finiteExtremum(
      fits, [](const FitQuality& fit) { return fit.inlier_ratio; }, std::less<double>());
}

double QualityMetrics::maximumFitResidualMm() const {
  return finiteExtremum(
      fits, [](const FitQuality& fit) { return fit.rms_residual_mm; }, std::greater<double>());
}

double QualityMetrics::minimumIcpScore() const {
  return finiteExtremum(
      icp,
      [](const IcpQuality& metric) {
        return metric.selected ? metric.score : std::numeric_limits<double>::quiet_NaN();
      },
      std::less<double>());
}

const char* toString(MeasurementStatus status) {
  switch (status) {
    case MeasurementStatus::kInactive:
      return "inactive";
    case MeasurementStatus::kSuccess:
      return "success";
    case MeasurementStatus::kFailure:
      return "failure";
  }
  return "failure";
}

const char* toString(FailureStage stage) {
  switch (stage) {
    case FailureStage::kNone:
      return "none";
    case FailureStage::kInput:
      return "input";
    case FailureStage::kCoordinateTransform:
      return "coordinate_transform";
    case FailureStage::kPreprocess:
      return "preprocess";
    case FailureStage::kSegmentation:
      return "segmentation";
    case FailureStage::kFiltering:
      return "filtering";
    case FailureStage::kTemplateLoading:
      return "template_loading";
    case FailureStage::kIcp:
      return "icp";
    case FailureStage::kRoi:
      return "roi";
    case FailureStage::kFitting:
      return "fitting";
    case FailureStage::kMeasurement:
      return "measurement";
    case FailureStage::kOutput:
      return "output";
  }
  return "none";
}

const char* toString(FailureCode code) {
  switch (code) {
    case FailureCode::kNone:
      return "none";
    case FailureCode::kFileMissing:
      return "file_missing";
    case FailureCode::kMalformedTuple:
      return "malformed_tuple";
    case FailureCode::kLengthMismatch:
      return "length_mismatch";
    case FailureCode::kNonFiniteValue:
      return "non_finite_value";
    case FailureCode::kEmptyCloud:
      return "empty_cloud";
    case FailureCode::kSplitFailed:
      return "split_failed";
    case FailureCode::kTemplateMissing:
      return "template_missing";
    case FailureCode::kIcpScoreLow:
      return "icp_score_low";
    case FailureCode::kRoiEmpty:
      return "roi_empty";
    case FailureCode::kInsufficientPoints:
      return "insufficient_points";
    case FailureCode::kLineFitFailed:
      return "line_fit_failed";
    case FailureCode::kCircleFitFailed:
      return "circle_fit_failed";
    case FailureCode::kRadiusOutOfRange:
      return "radius_out_of_range";
    case FailureCode::kArcCoverageLow:
      return "arc_coverage_low";
    case FailureCode::kUnsupportedGeometry:
      return "unsupported_geometry";
    case FailureCode::kInvalidConfiguration:
      return "invalid_configuration";
    case FailureCode::kInvalidGeometry:
      return "invalid_geometry";
    case FailureCode::kIoError:
      return "io_error";
    case FailureCode::kUnexpectedError:
      return "unexpected_error";
    case FailureCode::kModelRoiFailed:
      return "model_roi_failed";
  }
  return "unexpected_error";
}

std::string formatMeasurementValue(const MeasurementValue& value) {
  if (value.status == MeasurementStatus::kInactive) return "INACTIVE";
  if (value.status != MeasurementStatus::kSuccess || !value.value_mm ||
      !std::isfinite(*value.value_mm)) {
    return "NAN";
  }
  return formatFiniteNumber(*value.value_mm);
}

std::string formatFittedCircleRadiusMm(const DebugGeometry& geometry, const std::string& component,
                                       bool circle_enabled) {
  if (!circle_enabled) return "Disabled";
  const auto circle = geometry.circles.find(component);
  if (circle == geometry.circles.end() || circle->second.size() < 3) return "NAN";
  return formatFiniteNumber(circle->second[2] * 1000);
}

FailureCode classifyFailureCode(const std::string& message) {
  const auto text = lower(message);
  if (text.find("model roi") != std::string::npos) return FailureCode::kModelRoiFailed;
  if (text.find("file missing") != std::string::npos ||
      text.find("cannot open") != std::string::npos ||
      text.find("failed to open") != std::string::npos)
    return FailureCode::kFileMissing;
  if (text.find("tuple") != std::string::npos || text.find("parse") != std::string::npos)
    return FailureCode::kMalformedTuple;
  if (text.find("length") != std::string::npos || text.find("same size") != std::string::npos)
    return FailureCode::kLengthMismatch;
  if (text.find("non-finite") != std::string::npos || text.find("finite") != std::string::npos)
    return FailureCode::kNonFiniteValue;
  if (text.find("template") != std::string::npos) return FailureCode::kTemplateMissing;
  if (text.find("icp") != std::string::npos) return FailureCode::kIcpScoreLow;
  if (text.find("roi") != std::string::npos && text.find("point") != std::string::npos)
    return FailureCode::kRoiEmpty;
  if (text.find("line fit") != std::string::npos) return FailureCode::kLineFitFailed;
  if (text.find("circle fit") != std::string::npos) return FailureCode::kCircleFitFailed;
  if (text.find("arc coverage") != std::string::npos) return FailureCode::kArcCoverageLow;
  if (text.find("radius") != std::string::npos) return FailureCode::kRadiusOutOfRange;
  if (text.find("split") != std::string::npos) return FailureCode::kSplitFailed;
  if (text.find("insufficient") != std::string::npos ||
      text.find("point count") != std::string::npos)
    return FailureCode::kInsufficientPoints;
  if (text.find("no points") != std::string::npos || text.find("empty") != std::string::npos)
    return FailureCode::kEmptyCloud;
  if (text.find("not implemented") != std::string::npos) return FailureCode::kUnsupportedGeometry;
  if (text.find("configuration") != std::string::npos || text.find("must") != std::string::npos)
    return FailureCode::kInvalidConfiguration;
  return FailureCode::kUnexpectedError;
}

}  // namespace gap::core
