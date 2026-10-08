#pragma once
// glue 包的预编译头：OpenCV 的三个模块头与 nlohmann/json 每个 TU 都要拖一遍（与 lyflow_cv/cv_pch.h 同一个理由）。
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <nlohmann/json.hpp>

#include "lyflow/data.h"
#include "lyflow/operator.h"
#include "lyflow/overlay.h"
#include "lyflow/registry.h"
