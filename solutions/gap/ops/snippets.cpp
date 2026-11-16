// 随包的片段（m8-plan L14）。源文件在 solutions/gap/snippets/*.lyflow-snippet.json，
// 构建时由 lyflow_op_pack.cmake 按字节编进 gap_snippets.inc；这里逐份交给注册表，
// 算子是否注册、端口对不对由 Registry::validate() 在启动自检时查。
#include <string>

#include "lyflow/registry.h"

namespace lyflow::packs::gap {
namespace {

struct SnippetBlob {
  const char* file;
  const unsigned char* bytes;
  std::size_t size;
};

#include "gap_snippets.inc"

} // namespace

void registerSnippets(Registry& r) {
  for (const SnippetBlob* b = kSnippetBlobs; b->file; ++b) {
#if !defined(LYFLOW_GAP_MODEL) || !defined(LYFLOW_GAP_TEMPLATE)
    if (std::string(b->file) == "model-with-template-fallback.lyflow-snippet.json")
      continue;
#endif
    const std::string text(reinterpret_cast<const char*>(b->bytes), b->size);
    r.addSnippet(parseSnippet(text, std::string("solutions/gap/snippets/") + b->file));
  }
}

} // namespace lyflow::packs::gap
