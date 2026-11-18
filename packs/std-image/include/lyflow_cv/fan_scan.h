#pragma once

#include <set>
#include <string>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow_cv/line_scan.h"

namespace lyflow::std_image::inspection {

std::vector<Param> fanScanParams();
Status fanScanCompute(const line_scan::Field& response, const ParamView& params, Outputs& outputs);
std::vector<Issue> fanScanValidate(const ParamView& params, const std::set<std::string>& connected);

} // namespace lyflow::std_image::inspection
