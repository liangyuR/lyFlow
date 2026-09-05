#pragma once
// 一次运行。D3：异步 + 回调 + 可取消，抢占在 Rust 侧的 RunManager 做。
// 生命周期契约：start → cancel(可选、可多次) → join → free；join 返回后不再回调。
#include <atomic>
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
};

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

/// 只校验不执行，同步。返回诊断 JSON 数组。
std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir);

}  // namespace lyflow::exec
