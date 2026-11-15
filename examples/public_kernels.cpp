#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lyflow/kernel/constraints.h>
#include <lyflow/kernel/geometry.h>
#include <lyflow/kernel/metrology.h>
#include <lyflow/kernel/rigid.h>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
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
    require(near(distance, 5), "Meter geometry distance must be 5 m");
    Point2D midpoint;
    require(k::midpoint2D(a, b, midpoint, issue), issue.message);
    require(near(midpoint.p[0], 1.5) && near(midpoint.p[1], 2), "Incorrect meter midpoint");
    Line2D horizontal;
    horizontal.unit = Unit2D::Meter;
    horizontal.dir[0] = 1;
    horizontal.dir[1] = 0;
    Point2D projected;
    require(k::projectPoint2D(b, horizontal, false, projected, issue), issue.message);
    require(near(projected.p[0], 3) && near(projected.p[1], 0), "Incorrect line projection");
    Line2D vertical;
    vertical.unit = Unit2D::Meter;
    vertical.point[0] = 1;
    vertical.dir[0] = 0;
    vertical.dir[1] = 1;
    k::Intersections2D intersections;
    require(k::intersect2D(horizontal, vertical, 1e-9, intersections, issue), issue.message);
    require(intersections.points.size() == 1 && near(intersections.points[0].x, 1) &&
                near(intersections.points[0].y, 0),
            "Incorrect line intersection");
    Point2D pixelB = b;
    pixelB.unit = Unit2D::Pixel;
    require(!k::distance2D(a, pixelB, {}, distance, issue), "Mixed m/px geometry must be rejected");

    const Transform sourceToTarget = k::legacyEulerDegrees({1, 2, 3}, {0, 0, 90});
    k::M4 matrix;
    poses::Quaternion rotation;
    require(k::rigid(&sourceToTarget, matrix, rotation, why), why);
    const auto mapped = k::apply(matrix, {1, 0, 0});
    require(near(mapped[0], 1) && near(mapped[1], 3) && near(mapped[2], 3),
            "fixture->world point must be [1,3,3] m");
    const auto pose = poses::make3D("piece", sourceFrame, {1, 0, 0}, {0, 0, 0, 1}, {0.1, 0.2, 0.3});
    auto poseList = poses::make3DList(nlohmann::json::array({pose.data}), sourceFrame);
    const auto originalPoses = poseList.data;
    require(!k::transformPoses3D(poseList, nullptr, targetFrame, why) &&
                poseList.data == originalPoses,
            "Cross-frame pose mapping requires an explicit transform");
    require(k::transformPoses3D(poseList, &sourceToTarget, targetFrame, why), why);
    require(poseList.data["frame"] == targetFrame && poseList.data["unit"] == "m" &&
                near(poseList.data["items"][0]["position"][1].get<double>(), 3),
            "Pose frame or meter position was not preserved");
    Plane plane, transformedPlane;
    require(k::transformPlane(plane, sourceToTarget, transformedPlane, issue), issue.message);
    require(near(transformedPlane.d, -3), "Transformed z=0 plane must be z=3 m");

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
    require(constraints.size() == 3 && near(constraints[0].weight, 1) &&
                near(constraints[1].weight, 0.5),
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
    require(measurement.ok && measurement.unit == "m" && near(measurement.value, 0.01, 1e-12),
            "2 mm/px over 5 px must produce 0.01 m");
    require(!k::metrology::calibratedDistance(&calibration, &pixelA, &pixelC, "camera-B",
                                              measurement, why, port) &&
                near(measurement.value, 0.01, 1e-12),
            "Frame mismatch must preserve the previous measurement");
    k::metrology::ResidualAccumulator quality;
    require(std::isnan(quality.weightedRmse()), "Empty residuals must not report zero error");
    quality.add(0.003, 2);
    quality.add(0.004, 1);
    require(near(quality.weightedRmse(), std::sqrt(34.0 / 3.0) * 0.001, 1e-12) &&
                near(quality.maxError(), 0.004, 1e-12),
            "Weighted residual statistics must use observation weights");
    std::cout
        << "Static kernels OK: " << sourceFrame << "->" << targetFrame
        << " in m; 3 constraints validated (no pose solving); camera-A->table 0.01 m; weighted RMSE "
        << quality.weightedRmse() << " m\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Public kernel check failed: " << error.what() << '\n';
    return 1;
  }
}
