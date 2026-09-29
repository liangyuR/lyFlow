#pragma once
// 结果缓存的磁盘层（docs/disk-cache-plan.md）。CLI 专用、默认关：宿主经 lyflow_cache_set_dir 给目录与
// 构建指纹才打开。ResultStore 在内存里找不齐端口时来这里查，执行器算完贵的节点往这里写。
// 缓存判定仍只归 core（ADR-0007）：调用方只给目录，不决定哪个节点命中。
//
// 布局：<dir>/<fingerprint>/<cacheKey 前两位>/<cacheKey>.lfc，一个节点一个文件、全部端口在里面。
// 只落盘「每个端口都是点云或张量」的节点 —— 贵的上游（读盘、滤波、ONNX 推理）都在这两种上。
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/data.h"

namespace lyflow::exec {

using PortData = std::vector<std::pair<std::string, Data>>;

/// .lfc 的编解码。编码失败（有端口不是点云 / 张量）返回 false、out 不动。
bool encodeNode(const PortData& ports, std::string& out);
/// 解码失败（魔数、版本、长度对不上）返回 false。点云读回时重新分配 id（PointCloud 构造时自增）。
bool decodeNode(const std::string& bytes, PortData& out);
/// 这份 Data 能不能落盘。
bool persistable(const Data& d);

class DiskCache {
 public:
  static DiskCache& instance();

  /// dir 为空 = 关。返回空串 = 成功，否则一句人话的原因。
  std::string setDir(const std::string& dir, const std::string& fingerprint);
  bool enabled() const;

  /// 这个 cacheKey 在盘上有没有、端口是不是齐的（只读文件头）。
  bool has(const std::string& cacheKey, const std::vector<std::string>& ports) const;
  /// 读回全部端口。坏文件当未命中并删掉。
  bool load(const std::string& cacheKey, const std::vector<std::string>& ports, PortData& out) const;
  /// 写一个节点（临时文件 + 原子改名，多个进程并发写同一目录不会读到半个文件）。
  /// 有端口不能落盘就什么都不写。
  void store(const std::string& cacheKey, const PortData& ports) const;

  /// 耗时低于它的节点不落盘：读盘不一定比重算快。默认 20 ms；测试里设 0。
  void setMinDurationMs(double ms);
  double minDurationMs() const;

 private:
  DiskCache() = default;
  std::filesystem::path fileOf(const std::string& cacheKey) const;

  mutable std::mutex mu_;
  std::filesystem::path root_;  // <dir>/<fingerprint>；空 = 关
  double minDurationMs_ = 20.0;
};

}  // namespace lyflow::exec
