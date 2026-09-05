#pragma once
// 一次运行。D3：异步 + 回调 + 可取消，抢占在 Rust 侧的 RunManager 做。
// 生命周期契约：start → cancel(可选、可多次) → join → free；join 返回后不再回调。
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lyflow/c_api.h"

namespace lyflow::exec {

struct RunOptions {
  std::string runId;
  std::filesystem::path baseDir;
  std::vector<std::string> targets;
  /// 并行度。0 = min(4, 硬件线程数)。1 = 退回顺序执行。
  int maxParallel = 0;
  /// 结果仓字节预算。0 = 用默认值（min(8 GB, 物理内存 40%)）。
  std::uint64_t cacheBudgetBytes = 0;
};

/// 每个 worker 分到的内部并行度：max(1, cores / maxParallel)。
/// 算子（比如 PCL 的 OMP 版本）拿它当自己的线程数，免得超订。
int threadBudgetFor(int maxParallel);

/// maxParallel 的实际取值。0 → min(4, cores)。
int resolveMaxParallel(int requested);

class Run {
 public:
  Run(std::string graphJson, RunOptions options, lyflow_event_cb cb, void* user);
  ~Run();

  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;

  void cancel();
  void join();

  const std::string& runId() const { return options_.runId; }

 private:
  /// 线程函数体。只负责兜住异常，真正的活在 workImpl 里。
  void work();
  void workImpl();

  std::string graphJson_;
  RunOptions options_;
  lyflow_event_cb cb_ = nullptr;
  void* user_ = nullptr;
  std::atomic<bool> cancelled_{false};
  std::thread thread_;
  std::mutex joinMutex_;
  bool joined_ = false;
};

/// 只校验不执行，同步。返回诊断 JSON 数组（含迁移动作）。
std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir);

/// 编译一次并报告每个节点的 cacheKey / 是否已缓存（ADR-0007）。
/// 校验有错时返回诊断数组而不是计划数组，两者靠 kind 字段区分。
std::string planGraphJson(const std::string& graphJson, const std::filesystem::path& baseDir,
                          const std::vector<std::string>& targets);

}  // namespace lyflow::exec
