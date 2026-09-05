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

  std::size_t size() const;
  std::vector<std::string> dirs() const;

 private:
  Library() = default;

  mutable std::mutex mu_;
  std::map<std::string, SubgraphDef> defs_;
  std::vector<std::filesystem::path> dirs_;
};

}  // namespace lyflow::exec
