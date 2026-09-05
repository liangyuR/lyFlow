#include "exec/library.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "exec/subgraph.h"
#include "lyflow/registry.h"

namespace lyflow::exec {
namespace {

/// `<stem>.lyflow-op.json` 的 stem。文件名是 id 的兜底来源。
std::string stemOf(const std::filesystem::path& file) {
  std::string name = file.filename().u8string();
  const std::string suffix = kLibrarySuffix;
  if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(),
                                                  suffix) == 0) {
    name.resize(name.size() - suffix.size());
  }
  return name;
}

/// 单独校验一个合成算子。用一份只装了它的临时注册表，坏的那个不牵连其余。
std::vector<std::string> validateAlone(const OperatorDesc& op) {
  Registry probe;
  for (const auto& t : ensureRegistry().types()) probe.addType(t);
  probe.addOperator(op);
  return probe.validate();
}

}  // namespace

Library& Library::instance() {
  static Library lib;
  return lib;
}

std::vector<std::string> Library::setDirs(const std::vector<std::filesystem::path>& dirs) {
  std::vector<std::string> problems;
  std::map<std::string, SubgraphDef> defs;
  std::vector<OperatorDesc> operators;

  for (const auto& dir : dirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) continue;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::string name = entry.path().filename().u8string();
      const std::string suffix = kLibrarySuffix;
      if (name.size() <= suffix.size() ||
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        continue;
      }
      files.push_back(entry.path());
    }
    // 目录序在不同文件系统上不保证一致，排一下，manifest 才是稳定的
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
      std::ifstream in(file, std::ios::binary);
      if (!in) {
        problems.push_back("读不到库文件 " + file.u8string());
        continue;
      }
      nlohmann::json j;
      try {
        in >> j;
      } catch (const std::exception& e) {
        problems.push_back(file.u8string() + " 不是合法 JSON: " + e.what());
        continue;
      }
      std::string id = j.is_object() ? j.value("id", std::string()) : std::string();
      if (id.empty()) id = stemOf(file);
      const std::string opId = kLibraryIdPrefix + id;

      SubgraphDef def;
      std::string error;
      if (!parseSubgraphDef(j, id, def, error)) {
        problems.push_back(file.u8string() + ": " + error);
        continue;
      }
      if (def.category.empty()) def.category = "General";
      def.category = kLibraryCategoryPrefix + def.category;
      if (defs.count(opId)) {
        problems.push_back("库算子 id 重复: " + opId + "（" + file.u8string() + "）");
        continue;
      }
      OperatorDesc op = synthesizeOperator(def, opId);
      const auto bad = validateAlone(op);
      if (!bad.empty()) {
        for (const auto& b : bad) problems.push_back(file.u8string() + ": " + b);
        continue;
      }
      defs.emplace(opId, std::move(def));
      operators.push_back(std::move(op));
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    defs_ = std::move(defs);
    dirs_ = dirs;
  }
  ensureRegistry().setLibraryOperators(std::move(operators));
  return problems;
}

const SubgraphDef* Library::find(const std::string& opId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = defs_.find(opId);
  return it == defs_.end() ? nullptr : &it->second;
}

std::size_t Library::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return defs_.size();
}

std::vector<std::string> Library::dirs() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  for (const auto& d : dirs_) out.push_back(d.u8string());
  return out;
}

}  // namespace lyflow::exec
