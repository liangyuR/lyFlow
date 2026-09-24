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

#include "exec/graph.h"
#include "lyflow/c_api.h"
#include "lyflow/data.h"

namespace lyflow::exec {

/// 运行模式（F5）。preview 只改一件事：无输入的源算子先把输出抽稀。
enum class RunMode { Full = 0, Preview = 1 };

/// 运行时注入（ADR-0017）：这个节点的这个输出端口不由 compute 产出，由宿主直接给。
/// 一个节点只要被注入一次，它的**全部**输出端口都得给 —— compute 整个被跳过。
/// port 只是该算子的**输入**端口时是输入注入（m8-plan L18）：compute 照常跑，
/// 那个端口的值取注入数据；端口上不能同时有连线。
struct InjectedInput {
  std::string nodeId;
  std::string port;
  Data data;
};

struct RunOptions {
  std::string runId;
  std::filesystem::path baseDir;
  std::vector<std::string> targets;
  /// 只运行这些节点（node-run R1–R3）：给了它就忽略 targets、改用同一组 id；不在里面的
  /// 上游只许命中缓存，缺一个就在开跑前整次失败（upstream_not_ready），在里面的跳过缓存强制执行。
  /// 与 preview 模式互斥（R5）。
  std::vector<std::string> isolate;
  /// 并行度。0 = min(4, 硬件线程数)。1 = 退回顺序执行。
  int maxParallel = 0;
  /// 结果仓字节预算。0 = 用默认值（min(8 GB, 物理内存 40%)）。
  std::uint64_t cacheBudgetBytes = 0;
  RunMode mode = RunMode::Full;
  /// preview 模式下源算子输出的点数上限。0 = 用默认值 200000。
  std::uint32_t previewMaxPoints = 0;
  /// preview 超过这个耗时就发一条 warn 日志。0 = 用默认值 300。
  std::uint32_t previewBudgetMs = 0;
  /// 本次运行不复用结果仓里的旧结果（CLI 的 --no-cache）。仍然照常写入。
  bool noReuse = false;
  /// 运行时注入的源数据。摘要进 cacheKey。
  std::vector<InjectedInput> inputs;
  /// 顶层图参数的取值（J8），JSON 对象 名字→值；空串 = 全用 default。
  std::string paramsJson;
};

/// preview 的两个默认值。C ABI 传 0 表示「用默认」，两侧因此不必同步常量。
constexpr std::uint32_t kDefaultPreviewMaxPoints = 200000;
constexpr std::uint32_t kDefaultPreviewBudgetMs = 300;

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
/// paramsJson 与 RunOptions::paramsJson 同义：顶层图参数的取值，空串 = 全用 default。
std::string validateGraphJson(const std::string& graphJson,
                              const std::filesystem::path& baseDir,
                              const std::string& paramsJson = {});

/// parse + 盖上顶层参数取值 + expand 一步到位。任一步失败都返回 false，诊断已写进 diags。
bool prepareGraph(const std::string& graphJson, RawGraph& out, Diagnostics& diags,
                  const std::string& paramsJson = {});

/// 编译一次并报告每个节点的 cacheKey / 是否已缓存（ADR-0007）。
/// 校验有错时返回诊断数组而不是计划数组，两者靠 kind 字段区分。
std::string planGraphJson(const std::string& graphJson, const std::filesystem::path& baseDir,
                          const std::vector<std::string>& targets,
                          const std::string& paramsJson = {});

/// 每节点每参数的生效值与来源（m6-plan §2）。返回
/// `{ nodes: [ { node, op, params: [ { param, value, source, label?, unit?, min?, max? } ] } ] }`；
/// 校验有错时返回诊断数组（以 '[' 开头），与 planGraphJson 同一套区分办法。
/// source 四种：`default`（图里没写）/ `explicit`（图里写了）/ `bound`（子图提升参数灌进来的）/
/// `graph`（顶层图参数灌进来的，另带 graphParam 说是哪一个）。
std::string effectiveParamsJson(const std::string& graphJson,
                                const std::filesystem::path& baseDir,
                                const std::string& paramsJson = {});

/// 某次运行的图级命名输出（ADR-0017）。返回
/// `{ name: { node, port, type, elementCount, byteSize, value? } }`。
std::string runOutputsJson(const std::string& runId);

/// 某次运行的 run summary（ADR-0022）。执行器在发 run_finished 之前登记，
/// 所以 run 结束之前返回 false（out 不动）；run 被 free 之后也取不到了。
bool runSummaryJson(const std::string& runId, std::string& out);

/// 走注册好的导入器把一段文本变成图。失败时返回诊断 JSON 数组（以 '[' 开头），
/// 成功时返回 GraphDoc 对象（以 '{' 开头）。
std::string importGraphJson(const std::string& kind, const std::string& text,
                            const std::filesystem::path& baseDir);

}  // namespace lyflow::exec
