#pragma once

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <vector>

#include "lyflow/operator.h"

namespace lyflow::ops {
Status detectSTagHD15(const cv::Mat& gray, std::vector<std::vector<cv::Point2f>>& corners,
                      std::vector<int>& ids, ExecContext& ctx);
}
