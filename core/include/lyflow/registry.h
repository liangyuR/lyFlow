#pragma once
//
// 算子注册表。进程内唯一，启动时由 registerBuiltinOps() 填充。
//
#include <string>
#include <vector>

#include "lyflow/manifest.h"

namespace lyflow {

class Registry {
 public:
  static Registry& instance();

  void addType(PortType type);
  void addOperator(OperatorDesc op);
  void clear();

  const std::vector<PortType>& types() const { return types_; }
  const std::vector<OperatorDesc>& operators() const { return operators_; }

  const OperatorDesc* find(const std::string& id) const;
  const PortType* findType(const std::string& name) const;

  // 自检。在导出 manifest 前跑，把算子作者的笔误挡在这里，
  // 而不是让前端拿到一份自相矛盾的 manifest 再去猜。
  // 返回人类可读的问题列表，空 = 无问题。
  std::vector<std::string> validate() const;

  // 序列化为符合 schema/operator-manifest.schema.json 的 JSON。
  std::string toManifestJson() const;

 private:
  Registry() = default;

  std::vector<PortType> types_;
  std::vector<OperatorDesc> operators_;
};

// 注册所有内置算子与端口类型。
//
// 这里用显式调用列表，不用「静态对象自注册」。原因：core 是静态库，
// 链接器会丢掉没有任何符号被引用的 .obj，自注册的算子会静默消失，
// 而且是在 release 构建里才消失。要绕开得靠 /WHOLEARCHIVE 之类的链接器开关,
// 那是把正确性押在构建配置上。加一个算子多写一行调用，换来确定性，划算。
//
// 加新算子的完整流程：写 src/ops/xxx.cpp -> 在 builtin_ops.cpp 加一行。
// 前端不用动 —— 这是 ADR-0003 的承诺，也是这套设计的全部意义。
void registerBuiltinOps(Registry& registry);

}  // namespace lyflow
