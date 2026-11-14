#pragma once
#include <algorithm>
#include <cstddef>
#include <ostream>
#include <streambuf>
namespace lyflow_matching_runtime {
inline thread_local int budget = 1;
inline int threadCount() { return std::max(1, budget); }
class ThreadBudget {
public:
  explicit ThreadBudget(int count) : previous_(budget) { budget = std::max(1, count); }
  ~ThreadBudget() { budget = previous_; }
  ThreadBudget(const ThreadBudget&) = delete;
  ThreadBudget& operator=(const ThreadBudget&) = delete;

private:
  int previous_;
};
class NullBuffer : public std::streambuf {
  int_type overflow(int_type c) override { return traits_type::not_eof(c); }
};
inline std::ostream& diagnostics() {
  thread_local NullBuffer buffer;
  thread_local std::ostream stream(&buffer);
  return stream;
}
}
