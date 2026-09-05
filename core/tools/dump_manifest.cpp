//
// lyflow-dump-manifest —— 把算子 manifest 打到 stdout。
//
// 存在的理由：不启动整个 app 就能看见 C++ 侧到底导出了什么。
// 调 manifest 的时候不用每次都 tauri dev，也方便在 CI 里对着
// schema/operator-manifest.schema.json 直接校验。
//
//   lyflow-dump-manifest              # 打印 manifest
//   lyflow-dump-manifest --check      # 只跑自检，有问题时退出码非 0
//
#include <cstdio>
#include <cstring>
#include <string>

#include "lyflow/registry.h"
#include "lyflow/version.h"

int main(int argc, char** argv) {
  bool checkOnly = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--check") == 0) {
      checkOnly = true;
    } else if (std::strcmp(argv[i], "--version") == 0) {
      std::printf("lyflow-core %s\n", LYFLOW_VERSION);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }

  auto& r = lyflow::Registry::instance();
  lyflow::registerBuiltinOps(r);

  const auto problems = r.validate();
  if (!problems.empty()) {
    std::fprintf(stderr, "manifest self-check failed (%zu problem(s)):\n", problems.size());
    for (const auto& p : problems) std::fprintf(stderr, "  - %s\n", p.c_str());
    return 1;
  }

  if (checkOnly) {
    std::fprintf(stderr, "ok: %zu operator(s), %zu port type(s)\n",
                 r.operators().size(), r.types().size());
    return 0;
  }

  const std::string json = r.toManifestJson();
  std::fwrite(json.data(), 1, json.size(), stdout);
  std::fputc('\n', stdout);
  return 0;
}
