#pragma once
// 点云写盘的挂钩。core 不认识任何文件格式（ADR-0014），格式知识在标准包里；
// C ABI 的 lyflow_output_save 与 CLI 的 `lyflow dump` 经这里转交给它。
#include <filesystem>
#include <string>

#include "lyflow/data.h"
#include "lyflow/status.h"

namespace lyflow {

using CloudWriterFn = Status (*)(const PointCloud&, const std::filesystem::path&,
                                 const std::string&);

/// 装一个写盘实现。标准包在 registerPackOps 里调它；后装的覆盖先装的。
void setCloudWriter(CloudWriterFn fn);

/// 按扩展名选格式写一片点云。没有任何包装过写盘实现时返回 unsupported。
Status saveCloudToFile(const PointCloud& cloud, const std::filesystem::path& file,
                       const std::string& format);

}  // namespace lyflow
