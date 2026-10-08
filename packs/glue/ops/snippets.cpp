// 随包的片段（m8-plan L14）。源文件在 packs/glue/snippets/*.lyflow-snippet.json，构建时由
// lyflow_op_pack.cmake 按字节编进 glue_snippets.inc（照 packs/gap 的做法）。
#include <string>

#include "glue.h"

namespace lyflow::packs::glue {
namespace {

struct SnippetBlob {
  const char* file;
  const unsigned char* bytes;
  std::size_t size;
};

#include "glue_snippets.inc"

}  // namespace

void registerSnippets(Registry& r) {
  for (const SnippetBlob* b = kSnippetBlobs; b->file; ++b) {
    const std::string text(reinterpret_cast<const char*>(b->bytes), b->size);
    r.addSnippet(parseSnippet(text, std::string("packs/glue/snippets/") + b->file));
  }
}

}  // namespace lyflow::packs::glue
