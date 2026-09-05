#include "exec/result_store.h"

#include <algorithm>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace lyflow::exec {
namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;

std::string entryKey(const std::string& cacheKey, const std::string& port) {
  return cacheKey + ":" + port;
}

}  // namespace

std::uint64_t defaultCacheBudget() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    const std::uint64_t forty = static_cast<std::uint64_t>(status.ullTotalPhys * 0.4);
    return std::min<std::uint64_t>(8 * kGiB, forty);
  }
#endif
  return 2 * kGiB;
}

CachePin::CachePin(ResultStore& store, std::vector<std::string> keys)
    : store_(&store), keys_(std::move(keys)) {
  for (const auto& k : keys_) store_->pin(k);
}

CachePin::~CachePin() {
  if (!store_) return;
  for (const auto& k : keys_) store_->unpin(k);
}

void CachePin::add(const std::string& cacheKey) {
  if (!store_) return;
  store_->pin(cacheKey);
  keys_.push_back(cacheKey);
}

ResultStore& ResultStore::instance() {
  static ResultStore store;
  return store;
}

void ResultStore::setBudget(std::uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  budget_ = bytes;
  evictLocked();
}

std::uint64_t ResultStore::budget() const {
  std::lock_guard<std::mutex> lock(mu_);
  return budget_ ? budget_ : defaultCacheBudget();
}

void ResultStore::pin(const std::string& cacheKey) {
  std::lock_guard<std::mutex> lock(mu_);
  pins_[cacheKey] += 1;
}

void ResultStore::unpin(const std::string& cacheKey) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = pins_.find(cacheKey);
  if (it == pins_.end()) return;
  if (--it->second <= 0) pins_.erase(it);
}

void ResultStore::touchLocked(const std::string& key) {
  auto it = byKey_.find(key);
  if (it == byKey_.end()) return;
  lru_.erase(it->second.lru);
  lru_.push_back(key);
  it->second.lru = std::prev(lru_.end());
}

void ResultStore::evictLocked() {
  const std::uint64_t limit = budget_ ? budget_ : defaultCacheBudget();
  // 从最久没用的一头淘汰，跳过被活跃 run 钉住的。全被钉住时就超预算 ——
  // 正在算的那一份不能扔，扔了这次运行直接失败。
  for (auto it = lru_.begin(); bytes_ > limit && it != lru_.end();) {
    auto entry = byKey_.find(*it);
    if (entry == byKey_.end()) {
      it = lru_.erase(it);
      continue;
    }
    if (pins_.count(entry->second.cacheKey)) {
      ++it;
      continue;
    }
    bytes_ -= entry->second.bytes;
    byKey_.erase(entry);
    it = lru_.erase(it);
    counters_.evictions += 1;
  }
}

void ResultStore::put(const std::string& runId, const std::string& nodeId, const std::string& port,
                      const std::string& cacheKey, Data data) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string key = entryKey(cacheKey, port);
  auto it = byKey_.find(key);
  if (it == byKey_.end()) {
    Entry e;
    e.bytes = data.byteSize();
    e.cacheKey = cacheKey;
    e.data = std::move(data);
    lru_.push_back(key);
    e.lru = std::prev(lru_.end());
    bytes_ += e.bytes;
    byKey_.emplace(key, std::move(e));
    evictLocked();
  } else {
    touchLocked(key);  // 已经在仓里就是同一份内容，保留原来那个 shared_ptr
  }
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
  out = data->second.data;
  return true;
}

bool ResultStore::peek(const std::string& cacheKey,
                       const std::vector<std::string>& ports) const {
  if (cacheKey.empty() || ports.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& port : ports) {
    if (!byKey_.count(entryKey(cacheKey, port))) return false;
  }
  return true;
}

bool ResultStore::reuse(const std::string& runId, const std::string& nodeId,
                        const std::string& cacheKey, const std::vector<std::string>& ports,
                        std::vector<OutputInfo>& infos) {
  if (cacheKey.empty() || ports.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    counters_.misses += 1;
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& port : ports) {
    if (!byKey_.count(entryKey(cacheKey, port))) {
      counters_.misses += 1;
      return false;
    }
  }
  infos.clear();
  for (const auto& port : ports) {
    const std::string key = entryKey(cacheKey, port);
    const Entry& e = byKey_.at(key);
    infos.push_back(OutputInfo{port, e.data.typeName(), e.data.elementCount(), e.bytes});
    touchLocked(key);
    index_[runId][nodeId][port] = key;
  }
  counters_.hits += 1;
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
    out.push_back(OutputInfo{kv.first, data->second.data.typeName(),
                             data->second.data.elementCount(), data->second.bytes});
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
  // 只丢索引。Data 留着等下一次运行按 cacheKey 复用，超预算时由 LRU 淘汰。
  index_.erase(runId);
  evictLocked();
}

void ResultStore::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  index_.clear();
  byKey_.clear();
  lru_.clear();
  bytes_ = 0;
  counters_.hits = 0;
  counters_.misses = 0;
  counters_.evictions = 0;
}

std::size_t ResultStore::liveEntryCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return byKey_.size();
}

CacheStats ResultStore::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  CacheStats s = counters_;
  s.entries = byKey_.size();
  s.bytes = bytes_;
  s.budgetBytes = budget_ ? budget_ : defaultCacheBudget();
  return s;
}

}  // namespace lyflow::exec
