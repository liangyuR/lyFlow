#pragma once
// 结构化错误与诊断集合。paramPath/portName 让前端把红框标到具体的输入框或端口上；
// D5：校验一次返回全部诊断而不是第一个错误（docs/architecture.md）。
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lyflow {

enum class Phase { Validate, Compile, Execute };

const char* toString(Phase p);

/// 诊断的严重程度。warning 不阻止执行（比如算子次版本号变了），
/// 但要让用户看得见 —— 静默的兼容性降级是最难排查的一类问题。
enum class Severity { Error, Warning };

const char* toString(Severity s);

/// `code` 的取值集合与 schema/execution-event.schema.json 的 error.code 一致
/// （unknown_op / type_mismatch / bad_param / cycle / io / cancelled / internal …）。
struct Status {
  bool ok = true;
  Phase phase = Phase::Execute;
  /// 机器可读的短码
  std::string code;
  /// 给人看的一句话
  std::string message;
  /// 出错的参数名。前端据此定位输入框。
  std::string paramPath;
  /// 出错的端口名。
  std::string portName;

  static Status Ok() { return Status{}; }

  static Status Error(Phase phase, std::string code, std::string message,
                      std::string paramPath = {}, std::string portName = {}) {
    Status s;
    s.ok = false;
    s.phase = phase;
    s.code = std::move(code);
    s.message = std::move(message);
    s.paramPath = std::move(paramPath);
    s.portName = std::move(portName);
    return s;
  }

  /// 惰性端口的调度请求（ADR-0016）：执行器编译并跑完该端口的上游闭包，再重新调 compute。
  static Status Demand(std::string portName) {
    Status s;
    s.ok = false;
    s.phase = Phase::Execute;
    s.code = kDemandCode;
    s.message = "需要惰性端口 '" + portName + "' 的上游";
    s.portName = std::move(portName);
    return s;
  }

  bool isDemand() const { return !ok && code == kDemandCode; }

  static constexpr const char* kDemandCode = "demand";

  explicit operator bool() const { return ok; }
};

/// 算子 validate 钩子产出的一条问题。code/message/paramPath/portName 放在 status 里，
/// phase 由 buildPlan 统一改写成 Validate —— 钩子自己写什么都不算数。
struct Issue {
  Severity severity = Severity::Error;
  Status status;

  static Issue error(std::string code, std::string message, std::string paramPath = {},
                     std::string portName = {}) {
    return Issue{Severity::Error, Status::Error(Phase::Validate, std::move(code),
                                                std::move(message), std::move(paramPath),
                                                std::move(portName))};
  }
  static Issue warning(std::string code, std::string message, std::string paramPath = {},
                       std::string portName = {}) {
    return Issue{Severity::Warning, Status::Error(Phase::Validate, std::move(code),
                                                  std::move(message), std::move(paramPath),
                                                  std::move(portName))};
  }
};

/// 迁移诊断的载荷（ADR-0008）。C++ 只说「该改成什么」，写回 GraphDoc 是前端的事。
/// paramsJson 是完整的参数对象文本，不是补丁 —— 改名参数没法用补丁表达。
struct MigrationPlan {
  std::string op;
  std::string opVersion;
  std::string paramsJson = "{}";
  std::vector<std::string> notes;
};

/// 一条挂在某个节点上的诊断。nodeId 为空表示是整张图级别的问题。
struct Diagnostic {
  std::string nodeId;
  Severity severity = Severity::Error;
  Status status;
  /// 非空 = 这是一条迁移诊断，序列化时 kind 写成 "migration"。
  std::shared_ptr<MigrationPlan> migration;
};

/// 校验/编译的产物。顺序即产生顺序，前端按顺序展示。
class Diagnostics {
 public:
  void add(std::string nodeId, Status status, Severity severity = Severity::Error) {
    items_.push_back(Diagnostic{std::move(nodeId), severity, std::move(status), nullptr});
  }
  /// 迁移诊断。severity 是 warning：它不阻止执行，执行器在内存里已经用迁移后的值跑了。
  void migration(std::string nodeId, std::string message, MigrationPlan plan) {
    Status s = Status::Error(Phase::Validate, "migration", std::move(message));
    items_.push_back(Diagnostic{std::move(nodeId), Severity::Warning, std::move(s),
                                std::make_shared<MigrationPlan>(std::move(plan))});
  }
  void error(std::string nodeId, Phase phase, std::string code, std::string message,
             std::string paramPath = {}, std::string portName = {}) {
    add(std::move(nodeId),
        Status::Error(phase, std::move(code), std::move(message), std::move(paramPath),
                      std::move(portName)),
        Severity::Error);
  }
  void warn(std::string nodeId, Phase phase, std::string code, std::string message,
            std::string paramPath = {}) {
    add(std::move(nodeId),
        Status::Error(phase, std::move(code), std::move(message), std::move(paramPath)),
        Severity::Warning);
  }

  bool hasErrors() const {
    for (const auto& d : items_) {
      if (d.severity == Severity::Error) return true;
    }
    return false;
  }
  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }
  const std::vector<Diagnostic>& items() const { return items_; }

  /// 某个节点上的全部诊断（保持顺序）。node_state.errors 直接用它。
  std::vector<Diagnostic> forNode(const std::string& nodeId) const;

  /// 序列化成 JSON 数组。lyflow_validate 的返回值就是它。
  std::string toJson() const;

 private:
  std::vector<Diagnostic> items_;
};

}  // namespace lyflow
