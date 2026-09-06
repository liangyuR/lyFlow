#pragma once

// Header-only on purpose: gap_ml itself has no PCL link dependency (see CMakeLists.txt), so this
// converter only pulls PCL in for translation units that already have it (gap_batch_runner,
// process_controller). Consumers using the measurement-XY frame instead of the sensor-XZ frame
// (GUI's onnx_roi_engine.cpp) have their own converter and intentionally don't share this one.
#include "gap_ml/RoiFeatures.hpp"

#include <pcl/common/point_tests.h>
#include <pcl/point_cloud.h>

namespace gap::ml {

// Converts one sensor-frame (XZ) laser-profile cloud into the ProfileRow OnnxRoiPredictor expects:
// x_mm=point.x*1000, z_mm=point.z*1000, intensity=point.r (the RGB R channel), valid=
// pcl::isFinite(point) (all of x/y/z finite). x_mm/z_mm are written from the raw coordinate
// regardless of validity -- buildChannels re-sanitizes both against `valid` before use, so an
// invalid slot's raw (possibly NaN) coordinate is never read downstream.
template <typename PointT>
ProfileRow profileRowsFromSensorXzCloud(const pcl::PointCloud<PointT>& cloud) {
  ProfileRow row;
  row.x_mm.reserve(cloud.size());
  row.z_mm.reserve(cloud.size());
  row.intensity.reserve(cloud.size());
  row.valid.reserve(cloud.size());
  for (const auto& point : cloud.points) {
    row.x_mm.push_back(static_cast<double>(point.x) * 1000.0);
    row.z_mm.push_back(static_cast<double>(point.z) * 1000.0);
    row.intensity.push_back(static_cast<double>(point.r));
    row.valid.push_back(pcl::isFinite(point) ? 1 : 0);
  }
  return row;
}

}  // namespace gap::ml
