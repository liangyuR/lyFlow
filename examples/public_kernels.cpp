#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lyflow/kernel/constraints.h>
#include <lyflow/kernel/geometry.h>
#include <lyflow/kernel/metrology.h>
#include <lyflow/kernel/rigid.h>
#include <lyflow/kernel/profile.h>
#include <lyflow/kernel/profile_analysis.h>
#include <lyflow/kernel/profile_dimensions.h>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

bool finite(std::initializer_list<double> values) {
  for (double value : values)
    if (!std::isfinite(value))
      return false;
  return true;
}
} // namespace

int main() {
  try {
    using namespace lyflow;
    namespace k = lyflow::kernel;
    const std::string sourceFrame = "fixture", targetFrame = "world";
    const Point2D a{{0, 0}, Unit2D::Meter}, b{{3, 4}, Unit2D::Meter};
    k::GeometryIssue issue;
    std::string why;
    double distance = -1;
    require(k::validateGeometry2D(a, why), why);
    require(k::distance2D(a, b, {}, distance, issue), issue.message);
    require(std::isfinite(distance), "Meter geometry distance must be finite");
    Point2D midpoint;
    require(k::midpoint2D(a, b, midpoint, issue), issue.message);
    require(midpoint.unit == Unit2D::Meter && finite({midpoint.p[0], midpoint.p[1]}),
            "Midpoint must be a finite meter point");
    Line2D horizontal;
    horizontal.unit = Unit2D::Meter;
    horizontal.dir[0] = 1;
    horizontal.dir[1] = 0;
    Point2D projected;
    require(k::projectPoint2D(b, horizontal, false, projected, issue), issue.message);
    require(projected.unit == Unit2D::Meter && finite({projected.p[0], projected.p[1]}),
            "Line projection must be a finite meter point");
    Line2D vertical;
    vertical.unit = Unit2D::Meter;
    vertical.point[0] = 1;
    vertical.dir[0] = 0;
    vertical.dir[1] = 1;
    k::Intersections2D intersections;
    require(k::intersect2D(horizontal, vertical, 1e-9, intersections, issue), issue.message);
    require(!intersections.coincident && intersections.points.size() == 1 &&
                finite({intersections.points[0].x, intersections.points[0].y}),
            "Crossing lines must have one finite intersection");
    Point2D pixelB = b;
    pixelB.unit = Unit2D::Pixel;
    require(!k::distance2D(a, pixelB, {}, distance, issue), "Mixed m/px geometry must be rejected");

    const Transform sourceToTarget = k::legacyEulerDegrees({1, 2, 3}, {0, 0, 90});
    k::M4 matrix;
    poses::Quaternion rotation;
    require(k::rigid(&sourceToTarget, matrix, rotation, why), why);
    const auto mapped = k::apply(matrix, {1, 0, 0});
    require(finite({mapped[0], mapped[1], mapped[2]}), "fixture->world point must be finite");
    const auto pose = poses::make3D("piece", sourceFrame, {1, 0, 0}, {0, 0, 0, 1}, {0.1, 0.2, 0.3});
    auto poseList = poses::make3DList(nlohmann::json::array({pose.data}), sourceFrame);
    const auto originalPoses = poseList.data;
    require(!k::transformPoses3D(poseList, nullptr, targetFrame, why) &&
                poseList.data == originalPoses,
            "Cross-frame pose mapping requires an explicit transform");
    require(k::transformPoses3D(poseList, &sourceToTarget, targetFrame, why), why);
    const auto& position = poseList.data["items"][0]["position"];
    require(poseList.data["frame"] == targetFrame && poseList.data["unit"] == "m" &&
                position.size() == 3 &&
                finite({position[0].get<double>(), position[1].get<double>(),
                        position[2].get<double>()}),
            "Pose frame, unit or finite position was not preserved");
    Plane plane, transformedPlane;
    require(k::transformPlane(plane, sourceToTarget, transformedPlane, issue), issue.message);
    require(finite({transformedPlane.d, transformedPlane.normal[0], transformedPlane.normal[1],
                    transformedPlane.normal[2]}),
            "Transformed plane must be finite");

    const nlohmann::json items = nlohmann::json::array({
        {{"id", "origin"},
         {"type", "point"},
         {"source", {0, 0, 0}},
         {"target", {1, 2, 3}},
         {"weight", 2}},
        {{"id", "x-axis"},
         {"type", "point"},
         {"source", {1, 0, 0}},
         {"target", {1, 3, 3}},
         {"weight", 1}},
        {{"id", "y-axis"},
         {"type", "point"},
         {"source", {0, 1, 0}},
         {"target", {0, 2, 3}},
         {"weight", 1}},
    });
    nlohmann::json constraintData;
    k::constraints::Error error;
    require(k::constraints::construct3D(items.dump(), sourceFrame, targetFrame, "mixed",
                                        constraintData, error),
            error.message);
    std::vector<k::constraints::Constraint> constraints;
    require(k::constraints::parse3D(constraintData, sourceFrame, targetFrame, "mixed", constraints,
                                    error),
            error.message);
    require(constraints.size() == items.size(), "Every constraint item must be parsed");
    for (std::size_t i = 0; i < constraints.size(); ++i)
      require(constraints[i].id == items[i]["id"].get<std::string>() && constraints[i].weight > 0 &&
                  constraints[i].weight <= 1,
              "Constraint order or normalized weights changed");
    require(!k::constraints::parse3D(constraintData, sourceFrame, "other-world", "mixed",
                                     constraints, error),
            "Mismatched constraint frames must be rejected");
    auto millimeterConstraints = constraintData;
    millimeterConstraints["unit"] = "mm";
    require(!k::constraints::parse3D(millimeterConstraints, sourceFrame, targetFrame, "mixed",
                                     constraints, error),
            "The constraints contract requires explicit meters");

    const Record calibration{"image.PlaneCalib",
                             {{"schemaVersion", 1},
                              {"H", {2., 0., 0., 0., 2., 0., 0., 0., 1.}},
                              {"unit", "mm"},
                              {"imageFrame", "camera-A"},
                              {"planeFrame", "table"}}};
    const Point2D pixelA{{1, 2}, Unit2D::Pixel}, pixelC{{4, 6}, Unit2D::Pixel};
    Measurement measurement;
    std::string port;
    require(k::metrology::calibratedDistance(&calibration, &pixelA, &pixelC, "camera-A",
                                             measurement, why, port),
            why);
    require(measurement.ok && measurement.unit == "m" && std::isfinite(measurement.value),
            "Calibrated distance must be a finite meter measurement");
    const double calibrated = measurement.value;
    require(!k::metrology::calibratedDistance(&calibration, &pixelA, &pixelC, "camera-B",
                                              measurement, why, port) &&
                measurement.value == calibrated,
            "Frame mismatch must preserve the previous measurement");
    k::metrology::ResidualAccumulator quality;
    require(std::isnan(quality.weightedRmse()), "Empty residuals must not report zero error");
    quality.add(0.003, 2);
    quality.add(0.004, 1);
    require(finite({quality.weightedRmse(), quality.maxError()}),
            "Weighted residual statistics must be finite");
    k::profile::PlanarSamples planar;
    std::vector<float> xyz, channels;
    std::vector<std::uint8_t> colors;
    for (int i = 0; i < 101; ++i) {
      const float x = static_cast<float>(i * 0.1);
      planar.x.push_back(x);
      planar.z.push_back(2);
      xyz.insert(xyz.end(), {static_cast<float>(i * 0.0001), 0.002f, 0});
    }
    const auto pieces = k::profile::splitPieces(planar, 0.2, 1, 12);
    require(!pieces.empty(), "Ordered profile must form a piece");
    const auto view = k::profile::pieceView(planar, pieces.front());
    k::profile::Line millimeterLine;
    require(
        k::profile::fitLine(view, 0, view.size() - 1, millimeterLine) &&
            finite({millimeterLine.px, millimeterLine.pz, millimeterLine.dx, millimeterLine.dz}),
        "Profile kernel line fit failed");
    const k::profile_analysis::CloudView sampleView(xyz, channels, channels, colors);
    require(k::profile_analysis::validateCloud(sampleView, 12, why), why);
    k::profile_analysis::LineFit metricLine;
    require(k::profile_analysis::fitLine(sampleView, 0, 1, 0, 0.01, metricLine) &&
                finite({metricLine.center[0], metricLine.center[1], metricLine.rms}),
            "TLS analysis line fit failed");
    k::metrology::SignedPointLine signedDistance;
    require(k::metrology::signedPointLine({0.003, 0.004}, {0, 0.002}, {1, 0}, {0, 1},
                                          k::metrology::DirectionPolicy::Normalize, signedDistance,
                                          why),
            why);
    require(finite({signedDistance.distance, signedDistance.foot[0], signedDistance.foot[1]}),
            "Signed distance and foot must be finite");
    namespace dimensions = k::profile_dimensions;
    Line2D datum;
    datum.hasSegment = true;
    datum.start[0] = -0.001F;
    datum.end[0] = 0.001F;
    const Point2D reference{{0.004F, -0.003F}, Unit2D::Meter};
    dimensions::FlushOptions flushOptions;
    flushOptions.signedDistance = false;
    flushOptions.gain = -2;
    flushOptions.offsetMm = 0.5;
    dimensions::FlushResult flush;
    require(dimensions::flushPointLine(datum, reference, flushOptions, flush, why), why);
    require(std::isfinite(flush.valueMm), "Profile flush must be finite");
    Circle2D left, right;
    left.radius = 0.001F;
    right.center[0] = 0.01F;
    right.center[1] = 0.004F;
    right.radius = 0.002F;
    dimensions::CircleGapResult circleGap;
    dimensions::CircleGapOptions gapOptions;
    require(dimensions::circleGap(left, right, nullptr, gapOptions, circleGap, why), why);
    require(circleGap.valid && std::isfinite(circleGap.valueMm),
            "Center-line circle gap must be valid and finite");
    datum.start[0] = 0;
    datum.end[0] = 0.02F;
    gapOptions.definition = dimensions::CircleGapDefinition::DatumTangents;
    gapOptions.offsetMm = 0.25;
    require(dimensions::circleGap(left, right, &datum, gapOptions, circleGap, why), why);
    require(std::isfinite(circleGap.valueMm), "Datum tangent circle gap must be finite");
    dimensions::PointOffsetOptions offsetOptions;
    offsetOptions.gainDx = -2;
    offsetOptions.gainDy = 3;
    offsetOptions.absDx = offsetOptions.absDy = true;
    offsetOptions.dxOffsetMm = -0.5;
    offsetOptions.dyOffsetMm = 0.25;
    dimensions::PointOffsetResult offset;
    const Point2D offsetA{{0.001F, 0.002F}, Unit2D::Meter};
    const Point2D offsetB{{0.004F, -0.002F}, Unit2D::Meter};
    require(dimensions::pointOffset(offsetA, offsetB, offsetOptions, offset, why), why);
    require(finite({offset.dxMm, offset.dyMm, offset.distanceMm}),
            "Point offset components must be finite");
    std::cout << "Profile dimensions OK: flush " << flush.valueMm << " mm; tangent gap "
              << circleGap.valueMm << " mm; point offset " << offset.dxMm << '/' << offset.dyMm
              << " mm\n";
    require(dimensions::pointOffset(offsetA, offsetA, {}, offset, why),
            "Identical physical points must be accepted");
    std::cout << "Static kernels OK: " << sourceFrame << "->" << targetFrame << " in m; "
              << constraints.size() << " constraints validated (no pose solving); camera-A->table "
              << calibrated << " m; weighted RMSE " << quality.weightedRmse() << " m\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Public kernel check failed: " << error.what() << '\n';
    return 1;
  }
}
