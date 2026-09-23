#pragma once
// 算子注册表。进程内唯一，启动时由 registerBuiltinOps() 填充。
#include <string>
#include <vector>

#include "lyflow/data.h"
#include "lyflow/manifest.h"

namespace lyflow {

class Registry {
 public:
  static Registry& instance();

  void addType(PortType type);
  /// 声明一种 Bundle（m8-plan L2）。同 kind 重复声明时后者覆盖前者。
  void addBundle(BundleDesc bundle);
  void addOperator(OperatorDesc op);
  /// 注册一种「文本 → 图」的导入器（ADR-0017）。同 kind 重复注册时后者覆盖前者。
  void addImporter(ImporterDesc importer);
  void clear();

  /// 之后 addOperator 进来的算子归属哪个包（S7）。生成的注册入口在调包之前设、
  /// 调完清空；core 自己的算子因此留空。格式是「包名」或「包名@版本」。
  void setCurrentPack(std::string pack);

  /// 整份替换库算子（ADR-0010）。它们排在内置算子之后，其余处处与内置无差别。
  /// 与热重载同一条约定：调用时必须没有活跃的 run —— 会让旧的 OperatorDesc* 失效。
  void setLibraryOperators(std::vector<OperatorDesc> ops);

  const std::vector<PortType>& types() const { return types_; }
  const std::vector<BundleDesc>& bundles() const { return bundles_; }
  const std::vector<OperatorDesc>& operators() const { return operators_; }
  const std::vector<ImporterDesc>& importers() const { return importers_; }

  const OperatorDesc* find(const std::string& id) const;
  const PortType* findType(const std::string& name) const;
  const BundleDesc* findBundle(const std::string& kind) const;

  /// 端口类型名在这张注册表里认不认：类型表里的名字，或已声明 kind 的 `Bundle<kind>`。
  bool knowsType(const std::string& typeName) const;

  /// 按声明查一个 Bundle 值（m8-plan L2）。declaredType 是端口声明的 `Bundle<kind>`；
  /// 通过返回空串，否则返回一句人话（执行器把它报成 contract_violation）。
  /// expected / actual 填成契约违反要的「期望 vs 实际」。
  std::string checkBundle(const std::string& declaredType, const Data& value,
                          nlohmann::json* expected = nullptr,
                          nlohmann::json* actual = nullptr) const;
  const ImporterDesc* findImporter(const std::string& kind) const;

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
  std::vector<BundleDesc> bundles_;
  std::vector<OperatorDesc> operators_;
  std::vector<ImporterDesc> importers_;
  /// operators_ 里前多少个是内置的。setLibraryOperators 从这里往后重写。
  std::size_t builtinCount_ = 0;
  std::string currentPack_;
};

// 注册所有内置算子与端口类型。显式调用列表而非静态自注册，理由与加算子的流程
// 见 core/README.md「加一个算子」。
void registerBuiltinOps(Registry& registry);

/// 注册编进本次构建的算子包（ADR-0013 / ADR-0014）。两份实现都由 CMake 生成，
/// 没有对应的包时是空函数：std 是仓库内的 packs/*，external 是 LYFLOW_OP_PACKS。
void registerStdPacks(Registry& registry);
void registerExternalPacks(Registry& registry);

// 进程内那一份注册表，保证只填充一次。
// C ABI 入口、执行器、dump 工具、测试都从这里拿，避免两条路径各自初始化的竞态。
Registry& ensureRegistry();

}  // namespace lyflow
