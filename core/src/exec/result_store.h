#pragma once
//
// 结果仓（D4）。
//
// 两层结构，从第一天就是终态：
//
//   内容寻址层   cacheKey ──► Data        真正持有数据，同键只有一份
//   索引层       runId ──► (nodeId, port) ──► cacheKey
//
// 为什么不直接 runId+node+port ──► Data？因为 M3 要开缓存复用：两次运行里
// 参数没变的节点算出来的是同一个 cacheKey，直接共享同一份 shared_ptr，
// 到时候只需要「run_free 时不再删无人引用的 Data」+ 加一个 LRU 字节预算，
// 结构一行不用改。反过来如果 M2 按 runId 存数据，M3 要重写这一层和它的所有调用点。
//
// 点云绝不 JSON 化：一百万个点是 30MB 文本，前端还得整段解析一次。
// previewCloud 直接给出可以零拷贝进 Float32Array 的连续缓冲。
//
#include <cstdint>
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

class ResultStore {
 public:
  static ResultStore& instance();

  void put(const std::string& runId, const std::string& nodeId, const std::string& port,
           const std::string& cacheKey, Data data);

  bool get(const std::string& runId, const std::string& nodeId, const std::string& port,
           Data& out) const;

  std::vector<OutputInfo> outputsOf(const std::string& runId, const std::string& nodeId) const;

  /// 等步长抽样。maxPoints=0 视为不抽样。
  bool previewCloud(const std::string& runId, const std::string& nodeId, const std::string& port,
                    std::uint32_t maxPoints, CloudPreview& out) const;

  /// 释放一次运行的全部结果。M2 顺带删掉无人引用的 Data；
  /// M3 把「顺带删」换成 LRU 字节预算，其余不动。
  void freeRun(const std::string& runId);

  /// 只给测试用：清空一切。
  void clear();

  std::size_t liveEntryCount() const;

 private:
  ResultStore() = default;

  /// port -> cacheKey（带端口名的键：同一节点不同端口是不同的内容）
  using PortMap = std::map<std::string, std::string>;
  using NodeMap = std::map<std::string, PortMap>;

  mutable std::mutex mu_;
  std::unordered_map<std::string, Data> byKey_;
  std::unordered_map<std::string, NodeMap> index_;
};

}  // namespace lyflow::exec
