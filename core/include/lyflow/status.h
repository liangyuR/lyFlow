#pragma once
// 结构化错误与诊断集合。paramPath/portName 让前端把红框标到具体的输入框或端口上；
// D5：校验一次返回全部诊断而不是第一个错误（docs/architecture.md）。
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

  explicit operator bool() const { return ok; }
};

/// 一条挂在某个节点上的诊断。nodeId 为空表示是整张图级别的问题。
struct Diagnostic {
  std::string nodeId;
  Severity severity = Severity::Error;
  Status status;
};

/// 校验/编译的产物。顺序即产生顺序，前端按顺序展示。
class Diagnostics {
 public:
  void add(std::string nodeId, Status status, Severity severity = Severity::Error) {
    items_.push_back(Diagnostic{std::move(nodeId), severity, std::move(status)});
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
