#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lyflow/operator.h"

namespace lyflow::ml {

struct RuntimeOptions {
  std::filesystem::path modelPath;
  std::string inputName;
  std::string outputName;
  std::int64_t intraOpThreads = 1;
  std::string modelContract = "generic";
  std::string batchMode = "model";
};

std::string modelFingerprint(const std::filesystem::path& model);
Status inferTensor(const Tensor* input, const RuntimeOptions& options, Tensor& output,
                   ExecContext& context);
inline Status inferTensor(const Tensor& input, const RuntimeOptions& options, Tensor& output,
                          ExecContext& context) {
  return inferTensor(&input, options, output, context);
}

} // namespace lyflow::ml
