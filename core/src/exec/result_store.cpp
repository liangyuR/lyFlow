#include "exec/result_store.h"

#include <set>

namespace lyflow::exec {

ResultStore& ResultStore::instance() {
  static ResultStore store;
  return store;
}

void ResultStore::put(const std::string& runId, const std::string& nodeId, const std::string& port,
                      const std::string& cacheKey, Data data) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string key = cacheKey + ":" + port;
  byKey_.emplace(key, std::move(data));  // 已存在就是缓存命中，保留原来那份
  index_[runId][nodeId][port] = key;
}

bool ResultStore::get(const std::string& runId, const std::string& nodeId, const std::string& port,
                      Data& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto run = index_.find(runId);
  if (run == index_.end()) return false;
  auto node = run->second.find(nodeId);
  if (node == run->second.end()) return false;
  auto p = node->second.find(port);
  if (p == node->second.end()) return false;
  auto data = byKey_.find(p->second);
  if (data == byKey_.end()) return false;
  out = data->second;
  return true;
}

std::vector<OutputInfo> ResultStore::outputsOf(const std::string& runId,
                                               const std::string& nodeId) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<OutputInfo> out;
  auto run = index_.find(runId);
  if (run == index_.end()) return out;
  auto node = run->second.find(nodeId);
  if (node == run->second.end()) return out;
  for (const auto& kv : node->second) {
    auto data = byKey_.find(kv.second);
    if (data == byKey_.end()) continue;
    out.push_back(OutputInfo{kv.first, data->second.typeName(), data->second.elementCount(),
                             data->second.byteSize()});
  }
  return out;
}

bool ResultStore::previewCloud(const std::string& runId, const std::string& nodeId,
                               const std::string& port, std::uint32_t maxPoints,
                               CloudPreview& out) const {
  Data data;
  if (!get(runId, nodeId, port, data)) return false;
  const PointCloud* cloud = data.asCloud();
  if (!cloud) return false;

  const std::size_t total = cloud->pointCount();
  // 等步长而不是随机抽样：同一份数据两次预览必须长得一样，
  // 否则拖动 maxPoints 滑块时画面会「闪」，用户会以为数据在变。
  std::size_t step = 1;
  if (maxPoints > 0 && total > maxPoints) {
    step = (total + maxPoints - 1) / maxPoints;
  }

  out.totalPoints = static_cast<std::uint32_t>(total);
  out.hasIntensity = cloud->hasIntensity();
  out.xyz.clear();
  out.intensity.clear();
  out.xyz.reserve((total / step + 1) * 3);
  if (out.hasIntensity) out.intensity.reserve(total / step + 1);

  for (std::size_t i = 0; i < total; i += step) {
    out.xyz.push_back(cloud->xyz[i * 3]);
    out.xyz.push_back(cloud->xyz[i * 3 + 1]);
    out.xyz.push_back(cloud->xyz[i * 3 + 2]);
    if (out.hasIntensity) out.intensity.push_back(cloud->intensity[i]);
  }
  out.pointCount = static_cast<std::uint32_t>(out.xyz.size() / 3);

  // bounds 用**全量**点云算，不是抽样后的：视图 fit-to-bounds 时不该
  // 因为抽稀而漏掉边角上的点。
  const Bounds b = cloud->bounds();
  for (int i = 0; i < 3; ++i) {
    out.bounds[i] = b.valid ? b.min[i] : 0.0f;
    out.bounds[i + 3] = b.valid ? b.max[i] : 0.0f;
  }
  return true;
}

void ResultStore::freeRun(const std::string& runId) {
  std::lock_guard<std::mutex> lock(mu_);
  index_.erase(runId);

  // 顺带删无人引用的 Data。M3 换成 LRU 字节预算 —— 那时候「还有没有人引用」
  // 不再是删除的判据，「总字节数超没超」才是。
  std::set<std::string> referenced;
  for (const auto& run : index_) {
    for (const auto& node : run.second) {
      for (const auto& port : node.second) referenced.insert(port.second);
    }
  }
  for (auto it = byKey_.begin(); it != byKey_.end();) {
    it = referenced.count(it->first) ? std::next(it) : byKey_.erase(it);
  }
}

void ResultStore::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  index_.clear();
  byKey_.clear();
}

std::size_t ResultStore::liveEntryCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return byKey_.size();
}

}  // namespace lyflow::exec
