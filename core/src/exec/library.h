#pragma once
// 库算子目录（ADR-0010）。每个 `*.lyflow-op.json` 是一份独立存盘的子图定义，
// 扫描后注册成普通算子 `lib.<id>`，前端零改动就在面板的 Library/ 分类下看见它。
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "exec/graph.h"

namespace lyflow::exec {

/// 库文件后缀。用双扩展名是为了让通配符不会扫到普通的图文件。
constexpr const char* kLibrarySuffix = ".lyflow-op.json";

/// 库算子的 id 前缀与分类前缀。
constexpr const char* kLibraryIdPrefix = "lib.";
constexpr const char* kLibraryCategoryPrefix = "Library/";

class Library {
 public:
  static Library& instance();

  /// 重新扫描这些目录，替换掉全部库算子。返回人类可读的问题列表（空 = 干净）。
  /// 与热重载同一条约定：调用前调用方必须保证没有活跃的 run。
  std::vector<std::string> setDirs(const std::vector<std::filesystem::path>& dirs);

  /// 按算子 id（`lib.<x>`）找定义。返回的指针在下一次 setDirs 之前有效。
  const SubgraphDef* find(const std::string& opId) const;

  /// 库文件的原样内容去掉 `id` —— 正好是图文档里 `subgraphs.<id>` 的形状，编辑器「展开为内联子图」
  /// 直接写进 doc（docs/library-inline-plan.md）。category 是文件里写的，不带 `Library/` 前缀。
  /// 找不到返回空串。
  std::string definitionJson(const std::string& opId) const;

  std::size_t size() const;
  std::vector<std::string> dirs() const;

 private:
  Library() = default;

  mutable std::mutex mu_;
  std::map<std::string, SubgraphDef> defs_;
  /// 与 defs_ 同键：扫描时读到的原始 JSON（已去掉 id）。解析后的 SubgraphDef 丢了 ui 坐标等
  /// 编辑器要的东西，反序列化回去也不是逐键相同，所以原样留一份。
  std::map<std::string, nlohmann::json> raw_;
  std::vector<std::filesystem::path> dirs_;
};

}  // namespace lyflow::exec
