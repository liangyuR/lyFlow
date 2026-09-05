#include "lyflow/c_api.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "lyflow/registry.h"
#include "lyflow/version.h"

namespace {

// 注册表只填一次。桥接层可能从多个线程问 manifest（Tauri command 是并发的）。
lyflow::Registry& ensureRegistry() {
  static std::once_flag once;
  auto& r = lyflow::Registry::instance();
  std::call_once(once, [&] { lyflow::registerBuiltinOps(r); });
  return r;
}

// 用 malloc 而不是 new[]：跨 ABI 边界的内存必须能被 C 侧的 free 释放。
// 这里 Rust 走 lyflow_string_free 回来，但保持 malloc/free 配对是最少惊讶原则。
char* dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

}  // namespace

extern "C" {

const char* lyflow_version(void) { return LYFLOW_VERSION; }

char* lyflow_manifest_json(void) {
  return dup(ensureRegistry().toManifestJson());
}

char* lyflow_manifest_problems(void) {
  const auto problems = ensureRegistry().validate();
  std::string joined;
  for (const auto& p : problems) {
    if (!joined.empty()) joined.push_back('\n');
    joined += p;
  }
  return dup(joined);
}

void lyflow_string_free(char* s) { std::free(s); }

}  // extern "C"
