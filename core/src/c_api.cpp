#include "lyflow/c_api.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "exec/disk_cache.h"
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

/// 宿主注入的图像拷成 core 自己的一份（调用方的缓冲只活到 lyflow_run_start 返回），顺手去掉行填充。
/// 描述不合法时给一个不完整的 Image（没有像素）：执行器在注入的输出 / 输入上都会查 consistent()，
/// 报在那个节点上 —— 比在这里静默丢掉、再让下游报「没写输出」好懂。
lyflow::Image copyImageInput(const lyflow_run_image_input& in) {
  lyflow::Image img;
  img.width = static_cast<std::int32_t>(in.width);
  img.height = static_cast<std::int32_t>(in.height);
  img.channels = static_cast<std::int32_t>(in.channels);
  img.depth = static_cast<lyflow::PixelDepth>(in.depth);
  const bool shapeOk = in.width > 0 && in.height > 0 && in.width <= (1u << 20) &&
                       in.height <= (1u << 20) &&
                       (in.channels == 1 || in.channels == 3 || in.channels == 4) &&
                       (in.depth == 1 || in.depth == 2 || in.depth == 4);
  if (!shapeOk || in.pixels == nullptr) return img;
  const std::size_t tight = img.rowBytes();
  const std::size_t stride = in.row_bytes == 0 ? tight : in.row_bytes;
  if (stride < tight) return img;
  lyflow::Image out = lyflow::Image::allocate(img.width, img.height, img.channels, img.depth);
  for (std::uint32_t y = 0; y < in.height; ++y) {
    std::memcpy(out.mutablePixels() + y * tight, in.pixels + y * stride, tight);
  }
  return out;
}

/// LYIM 载荷的 48 字节头（docs/http-transport.md「图像」）。`.lyim` 文件与 HTTP 响应同一布局。
constexpr std::uint32_t kImageMagic = 0x4D49594Cu;  // 'LYIM' 小端

/// lyflow_output_image 的 handle：持有这一级的图像（level 0 就是结果仓里那一份的浅拷贝）。
struct ImageHold {
  lyflow::Image image;
};


/// 图像整张写成 `.lyim`：LYIM 载荷原样落盘（level 0、全部行）。别的扩展名拒掉 ——
/// PNG 这类要编解码器的格式归 std-image 的 io.save_image，core 不带。
std::string saveImageLyim(const lyflow::Image& img, const std::string& file) {
  const std::filesystem::path p = std::filesystem::u8path(file);
  std::string ext = p.extension().u8string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext != ".lyim") {
    return "图像输出只能存成 .lyim（PNG 等格式请在图里接 io.save_image），收到的是 '" + ext + "'";
  }
  if (!img.consistent()) return "图像不完整，没有可写的像素";
  const std::uint32_t head[12] = {
      kImageMagic,
      static_cast<std::uint32_t>(img.width),
      static_cast<std::uint32_t>(img.height),
      static_cast<std::uint32_t>(img.channels),
      static_cast<std::uint32_t>(img.depth),
      0u,  // level
      static_cast<std::uint32_t>(img.width),
      static_cast<std::uint32_t>(img.height),
      0u,  // row_offset
      static_cast<std::uint32_t>(img.height),
      static_cast<std::uint32_t>(img.rowBytes()),
      0u,
  };
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return "打不开输出文件: " + file;
  f.write(reinterpret_cast<const char*>(head), sizeof head);
  f.write(reinterpret_cast<const char*>(img.pixels.get()),
          static_cast<std::streamsize>(img.byteSize()));
  // 像素之后补齐到 4 字节，与 HTTP 载荷一致
  const std::size_t pad = (4 - img.byteSize() % 4) % 4;
  const char zeros[4] = {0, 0, 0, 0};
  f.write(zeros, static_cast<std::streamsize>(pad));
  if (!f) return "写输出文件失败: " + file;
  return std::string();
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

char* lyflow_library_definition(const char* op_id) {
  try {
    return dup(lyflow::exec::Library::instance().definitionJson(fromC(op_id)));
  } catch (...) {
    return dup(std::string());
  }
}

char* lyflow_validate(const char* graph_json, const char* base_dir) {
  return lyflow_validate_params(graph_json, base_dir, nullptr);
}

char* lyflow_validate_params(const char* graph_json, const char* base_dir,
                             const char* params_json) {
  try {
    registry();  // 确保注册表已填充
    const std::string base = fromC(base_dir);
    return dup(lyflow::exec::validateGraphJson(
        fromC(graph_json), base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base),
        fromC(params_json)));
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
  return lyflow_plan_params(graph_json, base_dir, targets, n, nullptr);
}

char* lyflow_plan_params(const char* graph_json, const char* base_dir, const char* const* targets,
                         size_t n, const char* params_json) {
  try {
    registry();
    const std::string base = fromC(base_dir);
    std::vector<std::string> ts;
    for (std::size_t i = 0; targets && i < n; ++i) ts.push_back(fromC(targets[i]));
    return dup(lyflow::exec::planGraphJson(
        fromC(graph_json),
        base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base), ts,
        fromC(params_json)));
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

char* lyflow_cache_set_dir(const char* dir, const char* fingerprint) {
  try {
    return dup(lyflow::exec::DiskCache::instance().setDir(fromC(dir), fromC(fingerprint)));
  } catch (const std::exception& e) {
    return dup(std::string("设置落盘缓存目录时内部异常: ") + e.what());
  } catch (...) {
    return dup(std::string("设置落盘缓存目录时内部异常"));
  }
}

void lyflow_cache_clear(void) {
  try {
    lyflow::exec::ResultStore::instance().clear();
  } catch (...) {
  }
}

char* lyflow_cache_evict(const char* graph_json, const char* base_dir,
                         const char* const* node_ids, size_t n, int include_downstream,
                         const char* params_json) {
  try {
    registry();
    const std::string base = fromC(base_dir);
    std::vector<std::string> ids;
    for (std::size_t i = 0; node_ids && i < n; ++i) ids.push_back(fromC(node_ids[i]));
    return dup(lyflow::exec::evictNodesJson(
        fromC(graph_json),
        base.empty() ? std::filesystem::path{} : std::filesystem::u8path(base), ids,
        include_downstream != 0, fromC(params_json)));
  } catch (const std::exception& e) {
    lyflow::Diagnostics d;
    d.error("", lyflow::Phase::Compile, "internal", std::string("清缓存时内部异常: ") + e.what());
    return dup(d.toJson());
  } catch (...) {
    return dup(std::string("[]"));
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
      for (std::size_t i = 0; opts->force && i < opts->force_count; ++i) {
        options.force.push_back(fromC(opts->force[i]));
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
      for (std::size_t i = 0; opts->image_inputs && i < opts->image_input_count; ++i) {
        options.inputs.push_back(lyflow::exec::InjectedInput{
            fromC(opts->image_inputs[i].node_id), fromC(opts->image_inputs[i].port),
            lyflow::Data::image(copyImageInput(opts->image_inputs[i]))});
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
                 (preview->hasNormals ? LYFLOW_CLOUD_HAS_NORMALS : 0u) |
                 (preview->hasRgb ? LYFLOW_CLOUD_HAS_RGB : 0u);
    std::memcpy(out->bounds, preview->bounds, sizeof(out->bounds));
    out->xyz = preview->xyz.data();
    out->intensity = preview->hasIntensity ? preview->intensity.data() : nullptr;
    out->normals = preview->hasNormals ? preview->normals.data() : nullptr;
    out->rgb = preview->hasRgb ? preview->rgb.data() : nullptr;
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


int lyflow_output_image(const char* run_id, const char* node_id, const char* port,
                        uint32_t level, uint32_t row_offset, uint32_t row_count,
                        lyflow_image_view* out) {
  if (!out) return 2;
  std::memset(out, 0, sizeof(*out));
  try {
    lyflow::Data data;
    if (!lyflow::exec::ResultStore::instance().get(fromC(run_id), fromC(node_id), fromC(port),
                                                   data)) {
      return 1;
    }
    const lyflow::Image* src = data.asImage();
    if (!src || !src->consistent()) return 1;

    // 要的级别太大时给「长边缩到 1 像素」那一级，而不是报错：前端按窗口大小算级别，算大了是常事
    // 尺寸向上取整，所以 2^level >= 长边时就是 1 像素
    const auto longest = static_cast<std::uint64_t>(std::max(src->width, src->height));
    unsigned maxLevel = 0;
    while ((std::uint64_t{1} << maxLevel) < longest) ++maxLevel;
    const unsigned lv = std::min<unsigned>(level, maxLevel);
    auto* held = new ImageHold{lyflow::shrinkImage(*src, lv)};
    const lyflow::Image& img = held->image;

    const std::uint32_t h = static_cast<std::uint32_t>(img.height);
    const std::uint32_t begin = row_offset > h ? h : row_offset;
    const std::uint32_t avail = h - begin;
    const std::uint32_t take = (row_count == 0 || row_count > avail) ? avail : row_count;

    out->width = static_cast<uint32_t>(img.width);
    out->height = h;
    out->channels = static_cast<uint32_t>(img.channels);
    out->depth = static_cast<uint32_t>(img.depth);
    out->level = lv;
    out->full_width = static_cast<uint32_t>(src->width);
    out->full_height = static_cast<uint32_t>(src->height);
    out->row_offset = row_offset;
    out->row_count = take;
    out->row_bytes = static_cast<uint32_t>(img.rowBytes());
    out->pixels = take == 0 ? nullptr : img.pixels.get() + std::size_t{begin} * img.rowBytes();
    out->handle = held;
    return 0;
  } catch (...) {
    return 3;
  }
}

void lyflow_image_view_free(lyflow_image_view* view) {
  if (!view || !view->handle) return;
  delete reinterpret_cast<ImageHold*>(view->handle);
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
    if (const lyflow::Image* img = data.asImage()) {
      return dup(saveImageLyim(*img, file));
    }
    const lyflow::PointCloud* cloud = data.asCloud();
    if (!cloud) {
      return dup(std::string("端口 ") + fromC(port) + " 不是点云或图像（是 " + data.typeName() + "）");
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
