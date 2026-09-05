#pragma once
// 算子注册表。进程内唯一，启动时由 registerBuiltinOps() 填充。
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

  // 自检。导出 manifest 前跑，把算子作者的笔误挡在这里。
  // 返回人类可读的问题列表，空 = 无问题。
  std::vector<std::string> validate() const;

  // 序列化为符合 schema/operator-manifest.schema.json 的 JSON。
  std::string toManifestJson() const;

  /// 进程内那一份走 instance()。允许自建是给测试和将来的子图用的 ——
  /// 一个隔离的注册表比「往全局塞个坏算子再想办法收拾」安全得多。
  Registry() = default;

 private:
  std::vector<PortType> types_;
  std::vector<OperatorDesc> operators_;
};

// 注册所有内置算子与端口类型。显式调用列表而非静态自注册，理由与加算子的流程
// 见 core/README.md「加一个算子」。
void registerBuiltinOps(Registry& registry);

// 进程内那一份注册表，保证只填充一次。
// C ABI 入口、执行器、dump 工具、测试都从这里拿，避免两条路径各自初始化的竞态。
Registry& ensureRegistry();

}  // namespace lyflow
