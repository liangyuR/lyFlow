#pragma once

#include <vector>

#include "lyflow_cv/line_scan.h"

namespace lyflow::std_image::line_scan {

std::vector<double> sampleCaliperStrip(const cv::Mat& gray, P2 center, P2 normal, double half,
                                       double width);
double parabolaPeak(double a, double b, double c);

} // namespace lyflow::std_image::line_scan
