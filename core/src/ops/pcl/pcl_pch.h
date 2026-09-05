#pragma once
//
// 只给 src/ops/pcl/ 下的 TU 用的预编译头。
//
// PCL 的头文件是模板 + Eigen 的重灾区，一个 TU 冷编要十几秒。把它们塞进 PCH，
// 一次付费，后面每个 PCL 算子只付自己那点代码的钱。
//
// **其余目录不吃这个 PCH**（D2）：手写算子和执行器不该为 PCL 的编译时间买单，
// 「加一个手写算子」的反馈循环必须保持秒级。
//
#include <pcl/PointIndices.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/io.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/sac_segmentation.h>
