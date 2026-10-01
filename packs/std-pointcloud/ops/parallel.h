#pragma once
// 逐点独立的循环按 ExecContext::threadBudget() 分段并行（法线、两个离群点滤波的近邻搜索）。
// vcpkg 的 PCL 没开 OpenMP：NormalEstimationOMP、StatisticalOutlierRemoval 里的 `#pragma omp` 在这里都是单线程，
// 200 万点上法线 12 s、统计离群点 14 s（core/README.md「并行」）。
//
// 规矩：每个点只写自己的那一格，彼此不碰；要跨点汇总的量由调用方在循环之后按原来的顺序单线程算 ——
// 浮点加法的顺序一变，结果就不再与线程数无关。
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lyflow::ops {

/// 每段的点数：够大才值得分给一个线程，也正好是看一次取消的间隔（同 ops::Ticker 的 8192）。
constexpr std::size_t kParallelBlock = 8192;

/// 把 [0, n) 切成 block 一段，threads 个线程轮流领下一段，各自调 body(begin, end)。
/// body 返回 false 表示看到了取消：别的线程领完手上这一段就停。threads ≤ 1 或只有一段时就在当前线程跑。
/// body 里抛的异常在这里收齐、回到调用线程上再抛 —— 从工作线程逃出去就是 std::terminate。
/// 逐点的循环用下面那个（block = kParallelBlock）；已经切成几十份粗块的（ASCII PCD 的行段）传 block = 1。
template <class Body>
void parallelFor(std::size_t n, int threads, std::size_t block, Body&& body) {
  const std::size_t blocks = (n + block - 1) / block;
  const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(std::max(1, threads)), blocks);
  std::atomic<std::size_t> next{0};
  std::atomic<bool> stop{false};
  std::exception_ptr error;
  std::mutex errorMu;
  auto work = [&] {
    try {
      while (!stop.load(std::memory_order_relaxed)) {
        const std::size_t b = next.fetch_add(1, std::memory_order_relaxed);
        if (b >= blocks) return;
        const std::size_t begin = b * block;
        if (!body(begin, std::min(n, begin + block))) stop.store(true, std::memory_order_relaxed);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(errorMu);
      if (!error) error = std::current_exception();
      stop.store(true, std::memory_order_relaxed);
    }
  };
  std::vector<std::thread> pool;
  if (count > 1) pool.reserve(count - 1);
  for (std::size_t t = 1; t < count; ++t) pool.emplace_back(work);
  work();
  for (auto& th : pool) th.join();
  if (error) std::rethrow_exception(error);
}

template <class Body>
void parallelFor(std::size_t n, int threads, Body&& body) {
  parallelFor(n, threads, kParallelBlock, std::forward<Body>(body));
}

}  // namespace lyflow::ops
