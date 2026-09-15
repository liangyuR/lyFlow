#pragma once
// 算子作者面对的全部接口。D6：参数校验与默认值合并集中在执行器，取值不会失败。
// 算子只需自己报两类错，见 core/README.md「写 compute 时的约定」。
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lyflow/data.h"
#include "lyflow/manifest.h"
#include "lyflow/status.h"

namespace lyflow {

/// 已校验并合并默认值的参数集合。键一定存在，类型一定对得上。
using ParamMap = std::unordered_map<std::string, Value>;

/// 参数读取视图。取不到值属于执行器的 bug，会抛异常而不是返回默认值 ——
/// 静默返回 0 会让一个 manifest 笔误表现成「算法结果不对」，那是最贵的一类 bug。
class ParamView {
 public:
  ParamView(const ParamMap& values, const std::filesystem::path& baseDir)
      : values_(&values), baseDir_(&baseDir) {}

  bool has(std::string_view name) const;

  bool flag(std::string_view name) const;
  std::int64_t integer(std::string_view name) const;
  double number(std::string_view name) const;
  const std::string& text(std::string_view name) const;
  /// enum 参数。返回 options 里的 value 字符串。
  const std::string& choice(std::string_view name) const;
  std::array<float, 2> vec2(std::string_view name) const;
  std::array<float, 3> vec3(std::string_view name) const;
  std::array<float, 4> vec4(std::string_view name) const;

  /// 相对路径按 baseDir 解析。空字符串原样返回空 path。
  std::filesystem::path path(std::string_view name) const;

  const std::filesystem::path& baseDir() const { return *baseDir_; }

  /// 给 externalKey 钩子用：拿到原始参数集合本身。
  const ParamMap& raw() const { return *values_; }

 private:
  const Value& lookup(std::string_view name) const;

  const ParamMap* values_;
  const std::filesystem::path* baseDir_;
};

/// 输入端口上的值。未连的可选端口 has() 返回 false。
class Inputs {
 public:
  explicit Inputs(const std::unordered_map<std::string, Data>& values) : values_(&values) {}

  bool has(std::string_view name) const;
  /// 端口不存在时返回一个 Kind::None 的静态空值 —— 必填端口已由执行器保证连上，
  /// 可选端口应当先 has() 再 get()。
  const Data& get(std::string_view name) const;

 private:
  const std::unordered_map<std::string, Data>* values_;
};

/// 输出端口。声明过的端口没写视为算子的 bug，执行器会报 output_not_written。
class Outputs {
 public:
  explicit Outputs(std::unordered_map<std::string, Data>& values) : values_(&values) {}
  void set(std::string_view name, Data value);

 private:
  std::unordered_map<std::string, Data>* values_;
};

enum class LogLevel { Debug, Info, Warn, Error };
const char* toString(LogLevel l);

/// 算子与执行器之间的回边：取消、进度、日志。
/// 取消是协作式的 —— 抢占式杀线程在有 PCL 时是纯粹的未定义行为来源。
class ExecContext {
 public:
  virtual ~ExecContext() = default;

  /// 在长循环里每隔几千个点问一次。返回 true 就尽快 return Status::Ok()
  /// 或任何值 —— 执行器会把该节点标成 cancelled，不看返回值。
  virtual bool cancelled() const = 0;

  virtual void progress(float ratio, std::string_view message = {}) = 0;
  virtual void log(LogLevel level, std::string message) = 0;

  /// 图文件所在目录。相对路径参数已经由 ParamView::path 解析过，
  /// 这里给需要自己拼路径的算子用。
  virtual const std::filesystem::path& baseDir() const = 0;

  /// 本节点可以自己开几个线程。执行器已经在跑 maxParallel 个节点，
  /// 算子内部再按核数开一遍就是超订（core/README.md「并行」）。
  virtual int threadBudget() const = 0;
};

/// 长循环里的取消轮询 + 进度上报。按 8192 个点问一次：
/// 每点一次会让原子读成为热点循环的瓶颈，而晚 8192 个点响应取消人感觉不到。
class Ticker {
 public:
  Ticker(ExecContext& ctx, std::size_t total) : ctx_(ctx), total_(total ? total : 1) {}

  /// 返回 true 表示应当立刻退出。
  bool tick(std::size_t i) {
    if ((i & 0x1FFF) != 0) return false;
    if (ctx_.cancelled()) return true;
    ctx_.progress(static_cast<float>(static_cast<double>(i) / static_cast<double>(total_)));
    return false;
  }

 private:
  ExecContext& ctx_;
  std::size_t total_;
};

}  // namespace lyflow
