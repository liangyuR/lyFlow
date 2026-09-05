#pragma once
// 结果仓（D4）。两层结构：内容寻址层 cacheKey→Data，索引层 runId→(nodeId,port)→cacheKey。
// 缓存判定的唯一权威在这里，前端只读它的结论（ADR-0007）。
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "lyflow/data.h"

namespace lyflow::exec {

struct CloudPreview {
  std::uint32_t pointCount = 0;   ///< 抽样后
  std::uint32_t totalPoints = 0;  ///< 抽样前
  bool hasIntensity = false;
  float bounds[6] = {0, 0, 0, 0, 0, 0};
  std::vector<float> xyz;
  std::vector<float> intensity;
};

struct OutputInfo {
  std::string port;
  std::string type;
  std::size_t elementCount = 0;
  std::size_t byteSize = 0;
};

struct CacheStats {
  std::size_t entries = 0;
  std::uint64_t bytes = 0;
  std::uint64_t budgetBytes = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
};

/// 一次运行期间把用到的 cacheKey 钉住，析构时解钉。
/// 没有它，一个大节点的输出可能在它的下游读到之前就被 LRU 淘汰掉。
class ResultStore;
class CachePin {
 public:
  CachePin() = default;
  CachePin(ResultStore& store, std::vector<std::string> keys);
  ~CachePin();
  CachePin(const CachePin&) = delete;
  CachePin& operator=(const CachePin&) = delete;

  void add(const std::string& cacheKey);

 private:
  ResultStore* store_ = nullptr;
  std::vector<std::string> keys_;
};

class ResultStore {
 public:
  static ResultStore& instance();

  void put(const std::string& runId, const std::string& nodeId, const std::string& port,
           const std::string& cacheKey, Data data);

  bool get(const std::string& runId, const std::string& nodeId, const std::string& port,
           Data& out) const;

  /// 缓存复用：cacheKey 下这些端口全都在仓里就把它们挂进本次运行的索引。
  /// 全有才算命中 —— 半份结果比没有更糟，下游会拿到一个空 Data。
  bool reuse(const std::string& runId, const std::string& nodeId, const std::string& cacheKey,
             const std::vector<std::string>& ports, std::vector<OutputInfo>& infos);

  /// 不改动命中计数的只读探测，lyflow_plan 用它预测 cached。
  bool peek(const std::string& cacheKey, const std::vector<std::string>& ports) const;

  std::vector<OutputInfo> outputsOf(const std::string& runId, const std::string& nodeId) const;

  /// 等步长抽样。maxPoints=0 视为不抽样。
  bool previewCloud(const std::string& runId, const std::string& nodeId, const std::string& port,
                    std::uint32_t maxPoints, CloudPreview& out) const;

  /// 只丢索引，Data 留在内容寻址层等下次复用（ADR-0007）。
  void freeRun(const std::string& runId);

  /// 清空一切。测试与「清空缓存」菜单用。
  void clear();

  std::size_t liveEntryCount() const;
  CacheStats stats() const;

  /// 字节预算。0 表示用默认值 min(8 GB, 物理内存 40%)。
  void setBudget(std::uint64_t bytes);
  std::uint64_t budget() const;

  void pin(const std::string& cacheKey);
  void unpin(const std::string& cacheKey);

 private:
  ResultStore() = default;

  struct Entry {
    Data data;
    /// 不带端口名的那一半，用来查 pins_
    std::string cacheKey;
    std::size_t bytes = 0;
    std::list<std::string>::iterator lru;
  };

  void touchLocked(const std::string& key);
  void evictLocked();

  /// port -> cacheKey（带端口名的键：同一节点不同端口是不同的内容）
  using PortMap = std::map<std::string, std::string>;
  using NodeMap = std::map<std::string, PortMap>;

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> byKey_;
  std::unordered_map<std::string, NodeMap> index_;
  /// 前 = 最久没用，后 = 刚用过
  std::list<std::string> lru_;
  /// cacheKey（不带端口）-> 钉住它的运行数
  std::unordered_map<std::string, int> pins_;
  std::uint64_t bytes_ = 0;
  std::uint64_t budget_ = 0;
  CacheStats counters_;
};

/// 默认预算：min(8 GB, 物理内存 40%)。取不到物理内存时退回 2 GB。
std::uint64_t defaultCacheBudget();

}  // namespace lyflow::exec
