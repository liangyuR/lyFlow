#pragma once
// 包里每个 TU 都要拖一遍这些头（PCL + Eigen + 领域头），不预编译一次增量重建十秒。
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <pcl/PointIndices.h>
#include <pcl/common/io.h>
#include <pcl/common/point_tests.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "domain/detection/DetectionConfiguration.hpp"
#include "domain/detection/DetectionTypes.hpp"
#include "gap_detection/GapUtils.hpp"

#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
