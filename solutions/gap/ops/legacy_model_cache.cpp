#include <memory>
#include <mutex>
#include <unordered_map>

#include "gap_ops.h"

namespace lyflow::packs::gap {
const ::gap::ml::OnnxRoiPredictor* predictorFor(const std::filesystem::path& model,
                                                std::string* message) {
  static std::mutex mu;
  static std::unordered_map<std::string, std::shared_ptr<::gap::ml::OnnxRoiPredictor>> cache;
  const std::string key = model.string() + "|" + fileStamp(model);
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second.get();
  try {
    auto made = std::make_shared<::gap::ml::OnnxRoiPredictor>(model);
    return cache.emplace(key, std::move(made)).first->second.get();
  } catch (const std::exception& e) {
    *message = std::string("模型加载失败: ") + e.what();
    return nullptr;
  }
}
}
