#include "lyflow/c_api.h"

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "exec/executor.h"
#include "exec/library.h"
#include "exec/result_store.h"
#include "ops/ops.h"
#include "lyflow/json_writer.h"
#include "lyflow/registry.h"
#include "lyflow/version.h"

namespace {

/// 进程级一次性初始化。setlocale(".UTF-8") 是 D9 的第二道保险：
/// cargo 生成的测试 exe 拿不到我们的 manifest，窄字符串 IO 只能靠这一句。
void ensureProcessInit() {
  static std::once_flag once;
  std::call_once(once, [] { std::setlocale(LC_ALL, ".UTF-8"); });
}

lyflow::Registry& registry() {
  ensureProcessInit();
  return lyflow::ensureRegistry();
}

// 用 malloc 而不是 new[]：跨 ABI 边界的内存必须能被 C 侧的 free 释放。
char* dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

std::string fromC(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

extern "C" {

const char* lyflow_version(void) { return LYFLOW_VERSION; }

char* lyflow_manifest_json(void) {
  try {
    return dup(registry().toManifestJson());
  } catch (...) {
    return nullptr;
  }
}

char* lyflow_manifest_problems(void) {
  try {
    const auto problems = registry().validate();
    std::string joined;
    for (const auto& p : problems) {
      if (!joined.empty()) joined.push_back('\n');
      joined += p;
    }
    return dup(joined);
  } catch (...) {
    return nullptr;
  }
}

void lyflow_string_free(char* s) { std::free(s); }

char* lyflow_set_library_dirs(const char* const* dirs, size_t n) {
  try {
    registry();
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 0; dirs && i < n; ++i) {
      const std::string d = fromC(dirs[i]);
      if (!d.empty()) paths.push_back(std::filesystem::u8path(d));
    }
    const auto problems = lyflow::exec::Library::instance().setDirs(paths);
    std::string joined;
    for (const auto& p : problems) {
      if (!joined.empty()) joined += "\n";
      joined += p;
    }
    return dup(joined);
  } catch (const std::exception& e) {
    return dup(std::string("扫描库目录时内部异常: ") + e.what());
  } catch (...) {
    return dup(std::string("扫描库目录时内部异常"));
  }
}

size_t lyflow_library_count(void) {
  try {
    return lyflow::exec::Library::instance().size();
  } catch (...) {
    return 0;
  }
}

char* lyflow_validate(const char* graph_json, const char* base_dir) {
  try {
    registry();  // 确保注册表已填充
    const std::string base = fromC(base_dir);
    return dup(lyflow::exec::validateGraphJson(
        fromC(graph_json), base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base)));
  } catch (const std::exception& e) {
    lyflow::Diagnostics d;
    d.error("", lyflow::Phase::Validate, "internal", std::string("校验时内部异常: ") + e.what());
    return dup(d.toJson());
  } catch (...) {
    return dup(std::string("[]"));
  }
}

char* lyflow_plan(const char* graph_json, const char* base_dir, const char* const* targets,
                  size_t n) {
  try {
    registry();
    const std::string base = fromC(base_dir);
    std::vector<std::string> ts;
    for (std::size_t i = 0; targets && i < n; ++i) ts.push_back(fromC(targets[i]));
    return dup(lyflow::exec::planGraphJson(
        fromC(graph_json),
        base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base), ts));
  } catch (const std::exception& e) {
    lyflow::Diagnostics d;
    d.error("", lyflow::Phase::Compile, "internal", std::string("编译时内部异常: ") + e.what());
    return dup(d.toJson());
  } catch (...) {
    return dup(std::string("[]"));
  }
}

void lyflow_cache_clear(void) {
  try {
    lyflow::exec::ResultStore::instance().clear();
  } catch (...) {
  }
}

char* lyflow_cache_stats(void) {
  try {
    const auto s = lyflow::exec::ResultStore::instance().stats();
    lyflow::JsonWriter w;
    w.beginObject();
    w.field("entries", static_cast<std::int64_t>(s.entries));
    w.field("bytes", static_cast<std::int64_t>(s.bytes));
    w.field("budgetBytes", static_cast<std::int64_t>(s.budgetBytes));
    w.field("hits", static_cast<std::int64_t>(s.hits));
    w.field("misses", static_cast<std::int64_t>(s.misses));
    w.field("evictions", static_cast<std::int64_t>(s.evictions));
    w.endObject();
    return dup(w.str());
  } catch (...) {
    return dup(std::string("{}"));
  }
}

lyflow_run* lyflow_run_start(const char* graph_json, const lyflow_run_options* opts,
                             lyflow_event_cb cb, void* user) {
  try {
    registry();
    lyflow::exec::RunOptions options;
    if (opts) {
      options.runId = fromC(opts->run_id);
      const std::string base = fromC(opts->base_dir);
      if (!base.empty()) options.baseDir = std::filesystem::u8path(base);
      for (std::size_t i = 0; opts->targets && i < opts->target_count; ++i) {
        options.targets.push_back(fromC(opts->targets[i]));
      }
      options.maxParallel = opts->max_parallel;
      options.cacheBudgetBytes = opts->cache_budget_bytes;
      options.mode = opts->mode == LYFLOW_RUN_MODE_PREVIEW ? lyflow::exec::RunMode::Preview
                                                           : lyflow::exec::RunMode::Full;
      options.previewMaxPoints = opts->preview_max_points;
      options.previewBudgetMs = opts->preview_budget_ms;
    }
    if (options.runId.empty()) options.runId = "run";
    return reinterpret_cast<lyflow_run*>(
        new lyflow::exec::Run(fromC(graph_json), std::move(options), cb, user));
  } catch (...) {
    return nullptr;
  }
}

void lyflow_run_cancel(lyflow_run* run) {
  if (!run) return;
  try {
    reinterpret_cast<lyflow::exec::Run*>(run)->cancel();
  } catch (...) {
  }
}

void lyflow_run_join(lyflow_run* run) {
  if (!run) return;
  try {
    reinterpret_cast<lyflow::exec::Run*>(run)->join();
  } catch (...) {
  }
}

void lyflow_run_free(lyflow_run* run) {
  if (!run) return;
  try {
    delete reinterpret_cast<lyflow::exec::Run*>(run);
  } catch (...) {
  }
}

int lyflow_output_cloud(const char* run_id, const char* node_id, const char* port,
                        uint32_t max_points, lyflow_cloud_view* out) {
  if (!out) return 2;
  std::memset(out, 0, sizeof(*out));
  try {
    auto preview = new lyflow::exec::CloudPreview();
    if (!lyflow::exec::ResultStore::instance().previewCloud(fromC(run_id), fromC(node_id),
                                                           fromC(port), max_points, *preview)) {
      delete preview;
      return 1;
    }
    out->point_count = preview->pointCount;
    out->total_points = preview->totalPoints;
    out->flags = (preview->hasIntensity ? LYFLOW_CLOUD_HAS_INTENSITY : 0u) |
                 (preview->hasNormals ? LYFLOW_CLOUD_HAS_NORMALS : 0u);
    std::memcpy(out->bounds, preview->bounds, sizeof(out->bounds));
    out->xyz = preview->xyz.data();
    out->intensity = preview->hasIntensity ? preview->intensity.data() : nullptr;
    out->normals = preview->hasNormals ? preview->normals.data() : nullptr;
    out->handle = preview;
    return 0;
  } catch (...) {
    return 3;
  }
}

void lyflow_cloud_view_free(lyflow_cloud_view* view) {
  if (!view || !view->handle) return;
  delete reinterpret_cast<lyflow::exec::CloudPreview*>(view->handle);
  std::memset(view, 0, sizeof(*view));
}

char* lyflow_output_info(const char* run_id, const char* node_id) {
  try {
    const auto infos = lyflow::exec::ResultStore::instance().outputsOf(fromC(run_id), fromC(node_id));
    lyflow::JsonWriter w;
    w.beginArray();
    for (const auto& i : infos) {
      w.beginObject();
      w.field("port", i.port);
      w.field("type", i.type);
      w.field("elementCount", static_cast<std::int64_t>(i.elementCount));
      w.field("byteSize", static_cast<std::int64_t>(i.byteSize));
      if (!i.valueJson.empty()) {
        w.key("value");
        w.raw(i.valueJson);
      }
      w.endObject();
    }
    w.endArray();
    return dup(w.str());
  } catch (...) {
    return dup(std::string("[]"));
  }
}

char* lyflow_output_save(const char* run_id, const char* node_id, const char* port,
                         const char* path, const char* format) {
  try {
    const std::string file = fromC(path);
    if (file.empty()) return dup(std::string("没有给输出路径"));
    lyflow::Data data;
    if (!lyflow::exec::ResultStore::instance().get(fromC(run_id), fromC(node_id), fromC(port),
                                                   data)) {
      return dup(std::string("结果仓里没有 ") + fromC(node_id) + "." + fromC(port));
    }
    const lyflow::PointCloud* cloud = data.asCloud();
    if (!cloud) {
      return dup(std::string("端口 ") + fromC(port) + " 不是点云（是 " + data.typeName() + "）");
    }
    const std::string fmt = fromC(format);
    const lyflow::Status s = lyflow::ops::saveCloudToFile(
        *cloud, std::filesystem::u8path(file), fmt.empty() ? std::string("binary") : fmt);
    return dup(s.ok ? std::string() : s.message);
  } catch (const std::exception& e) {
    return dup(std::string("写盘时内部异常: ") + e.what());
  } catch (...) {
    return dup(std::string("写盘时内部异常"));
  }
}

}  // extern "C"
