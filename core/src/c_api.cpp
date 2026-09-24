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
#include "lyflow/cloud_io.h"
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

char* lyflow_effective_params(const char* graph_json, const char* base_dir) {
  try {
    registry();
    const std::string base = fromC(base_dir);
    return dup(lyflow::exec::effectiveParamsJson(
        fromC(graph_json),
        base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base)));
  } catch (const std::exception& e) {
    lyflow::Diagnostics d;
    d.error("", lyflow::Phase::Compile, "internal",
            std::string("求生效参数时内部异常: ") + e.what());
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
      for (std::size_t i = 0; opts->isolate && i < opts->isolate_count; ++i) {
        options.isolate.push_back(fromC(opts->isolate[i]));
      }
      options.maxParallel = opts->max_parallel;
      options.cacheBudgetBytes = opts->cache_budget_bytes;
      options.mode = opts->mode == LYFLOW_RUN_MODE_PREVIEW ? lyflow::exec::RunMode::Preview
                                                           : lyflow::exec::RunMode::Full;
      options.previewMaxPoints = opts->preview_max_points;
      options.previewBudgetMs = opts->preview_budget_ms;
      options.noReuse = opts->no_reuse != 0;
      options.paramsJson = fromC(opts->params_json);
      for (std::size_t i = 0; opts->inputs && i < opts->input_count; ++i) {
        const lyflow_run_input& in = opts->inputs[i];
        if (in.kind != LYFLOW_INPUT_POINT_CLOUD) continue;
        lyflow::PointCloud cloud;
        // 调用方的缓冲只保证活到本函数返回，所以在这里就拷成 core 自己的。
        if (in.xyz && in.count) {
          cloud.xyz.assign(in.xyz, in.xyz + static_cast<std::size_t>(in.count) * 3);
        }
        if (in.intensity && in.count) {
          cloud.intensity.assign(in.intensity, in.intensity + in.count);
        }
        if (in.normals && in.count) {
          cloud.normals.assign(in.normals, in.normals + static_cast<std::size_t>(in.count) * 3);
        }
        if (in.rgb && in.count) {
          cloud.rgb.assign(in.rgb, in.rgb + static_cast<std::size_t>(in.count) * 3);
        }
        options.inputs.push_back(lyflow::exec::InjectedInput{
            fromC(in.node_id), fromC(in.port), lyflow::Data::cloud(std::move(cloud))});
      }
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

int lyflow_output_tensor(const char* run_id, const char* node_id, const char* port,
                         uint64_t offset, uint32_t count, lyflow_tensor_view* out) {
  if (!out) return 2;
  std::memset(out, 0, sizeof(*out));
  try {
    lyflow::Data data;
    if (!lyflow::exec::ResultStore::instance().get(fromC(run_id), fromC(node_id), fromC(port),
                                                   data)) {
      return 1;
    }
    const lyflow::Tensor* tensor = data.asTensor();
    if (!tensor) return 1;

    const std::uint64_t total = static_cast<std::uint64_t>(tensor->data.size());
    const std::uint64_t begin = offset > total ? total : offset;
    const std::uint64_t avail = total - begin;
    std::uint64_t take = (count == 0 || static_cast<std::uint64_t>(count) > avail)
                             ? avail
                             : static_cast<std::uint64_t>(count);
    if (take > 0xFFFFFFFFull) take = 0xFFFFFFFFull;

    auto* held = new lyflow::Data(data);
    out->rank = static_cast<uint32_t>(tensor->shape.size());
    out->count = static_cast<uint32_t>(take);
    out->offset = offset;
    out->total = total;
    out->shape = tensor->shape.empty() ? nullptr : tensor->shape.data();
    out->data = take == 0 ? nullptr : tensor->data.data() + static_cast<std::size_t>(begin);
    out->handle = held;
    return 0;
  } catch (...) {
    return 3;
  }
}

void lyflow_tensor_view_free(lyflow_tensor_view* view) {
  if (!view || !view->handle) return;
  delete reinterpret_cast<lyflow::Data*>(view->handle);
  std::memset(view, 0, sizeof(*view));
}

int lyflow_output_indices(const char* run_id, const char* node_id, const char* port,
                          uint64_t offset, uint32_t count, lyflow_indices_view* out) {
  if (!out) return 2;
  std::memset(out, 0, sizeof(*out));
  try {
    lyflow::Data data;
    if (!lyflow::exec::ResultStore::instance().get(fromC(run_id), fromC(node_id), fromC(port),
                                                   data)) {
      return 1;
    }
    const lyflow::Indices* indices = data.asIndices();
    if (!indices) return 1;

    const std::uint64_t total = static_cast<std::uint64_t>(indices->values.size());
    const std::uint64_t begin = offset > total ? total : offset;
    const std::uint64_t avail = total - begin;
    std::uint64_t take = (count == 0 || static_cast<std::uint64_t>(count) > avail)
                             ? avail
                             : static_cast<std::uint64_t>(count);
    if (take > 0xFFFFFFFFull) take = 0xFFFFFFFFull;

    auto* held = new lyflow::Data(data);
    out->count = static_cast<uint32_t>(take);
    out->total = static_cast<uint32_t>(total > 0xFFFFFFFFull ? 0xFFFFFFFFull : total);
    out->source_cloud_id = indices->sourceCloudId;
    out->values = take == 0 ? nullptr : indices->values.data() + static_cast<std::size_t>(begin);
    out->handle = held;
    return 0;
  } catch (...) {
    return 3;
  }
}

void lyflow_indices_view_free(lyflow_indices_view* view) {
  if (!view || !view->handle) return;
  delete reinterpret_cast<lyflow::Data*>(view->handle);
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

char* lyflow_run_outputs(const char* run_id) {
  try {
    return dup(lyflow::exec::runOutputsJson(fromC(run_id)));
  } catch (...) {
    return dup(std::string("{}"));
  }
}

char* lyflow_run_summary(const char* run_id) {
  try {
    std::string summary;
    // run 还没结束（或者已经被 free）时没有这一份：返回 NULL 而不是空对象 ——
    // "{}" 会被消费方当成「跑完了、什么都没有」，那是另一件事。
    if (!lyflow::exec::runSummaryJson(fromC(run_id), summary)) return nullptr;
    return dup(summary);
  } catch (...) {
    return nullptr;
  }
}

char* lyflow_import(const char* kind, const char* text, const char* base_dir) {
  try {
    registry();
    const std::string base = fromC(base_dir);
    return dup(lyflow::exec::importGraphJson(
        fromC(kind), fromC(text),
        base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base)));
  } catch (const std::exception& e) {
    lyflow::Diagnostics d;
    d.error("", lyflow::Phase::Validate, "internal", std::string("导入时内部异常: ") + e.what());
    return dup(d.toJson());
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
    const lyflow::Status s = lyflow::saveCloudToFile(
        *cloud, std::filesystem::u8path(file), fmt.empty() ? std::string("binary") : fmt);
    return dup(s.ok ? std::string() : s.message);
  } catch (const std::exception& e) {
    return dup(std::string("写盘时内部异常: ") + e.what());
  } catch (...) {
    return dup(std::string("写盘时内部异常"));
  }
}

}  // extern "C"
