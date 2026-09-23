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
  bool hasNormals = false;
  float bounds[6] = {0, 0, 0, 0, 0, 0};
  std::vector<float> xyz;
  std::vector<float> intensity;
  /// 3 * pointCount，或空。3D 视图的「法线着色」靠它（M3 尾巴 c）。
  std::vector<float> normals;
};

struct OutputInfo {
  std::string port;
  std::string type;
  std::size_t elementCount = 0;
  std::size_t byteSize = 0;
  /// 非点云输出的可读 JSON（Data::valueJson）。点云/Indices 为空串。
  std::string valueJson;
};

/// Bundle 端口按字段展开（m8-plan L3）：在 out 末尾追加 `<port>.<field>` 各一项。
/// 不是 Bundle 就什么都不做。事件、lyflow_output_info、缓存复用三处共用它。
void appendBundleFieldInfos(const std::string& port, const Data& data,
                            std::vector<OutputInfo>& out);

/// `<port>.<field>` 拆成两半。没有点返回 false。字段名里不许有点（Registry::validate），
/// 端口名里可以没有限制，所以按**最后**一个点拆。
bool splitFieldAddress(const std::string& address, std::string* port, std::string* field);

/// 一条图级命名输出的登记（ADR-0017）。执行器编译完就登记，宿主按名字取。
struct NamedOutput {
  std::string name;
  std::string nodeId;
  std::string port;
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

  /// port 也可以写成 `<port>.<field>`：取 Bundle 端口里的那个字段（m8-plan L3）。
  /// lyflow_output_cloud / tensor / indices / save 都经这里，所以签名不变就认这种写法。
  bool get(const std::string& runId, const std::string& nodeId, const std::string& port,
           Data& out) const;

  /// 缓存复用：cacheKey 下这些端口全都在仓里就把它们挂进本次运行的索引。
  /// 全有才算命中 —— 半份结果比没有更糟，下游会拿到一个空 Data。
  bool reuse(const std::string& runId, const std::string& nodeId, const std::string& cacheKey,
             const std::vector<std::string>& ports, std::vector<OutputInfo>& infos);

  /// 不改动命中计数的只读探测，lyflow_plan 用它预测 cached。
  bool peek(const std::string& cacheKey, const std::vector<std::string>& ports) const;

  /// 某节点的全部输出。Bundle 端口后面紧跟它的每个字段（`<port>.<field>`）。
  std::vector<OutputInfo> outputsOf(const std::string& runId, const std::string& nodeId) const;

  void setNamedOutputs(const std::string& runId, std::vector<NamedOutput> outputs);
  std::vector<NamedOutput> namedOutputs(const std::string& runId) const;

  /// run summary（ADR-0022）。执行器在发 run_finished 之前登记，
  /// `lyflow_run_summary` 按 runId 取；run 结束之前取不到，返回 false。
  void setSummary(const std::string& runId, std::string json);
  bool summary(const std::string& runId, std::string& out) const;

  /// 单个端口的元信息（类型、元素数、字节数、可读值）。没有该结果时返回 false。
  bool outputInfo(const std::string& runId, const std::string& nodeId, const std::string& port,
                  OutputInfo& out) const;

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
  /// get 的无锁版本，只认端口名本身（不拆字段）。
  bool getLocked(const std::string& runId, const std::string& nodeId, const std::string& port,
                 Data& out) const;
  void evictLocked();

  /// port -> cacheKey（带端口名的键：同一节点不同端口是不同的内容）
  using PortMap = std::map<std::string, std::string>;
  using NodeMap = std::map<std::string, PortMap>;

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> byKey_;
  std::unordered_map<std::string, NodeMap> index_;
  std::unordered_map<std::string, std::vector<NamedOutput>> namedOutputs_;
  /// runId -> run summary 的 JSON 文本（ADR-0022）。与索引同生共死。
  std::unordered_map<std::string, std::string> summaries_;
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
