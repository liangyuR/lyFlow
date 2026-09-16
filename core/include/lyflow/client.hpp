#ifndef LYFLOW_CLIENT_HPP
#define LYFLOW_CLIENT_HPP
// 嵌入 SDK：header-only 的 C++ 封装，只依赖 lyflow/c_api.h。
// 用法、安装布局与线程约定见 docs/embedding.md。
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/c_api.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace lyflow {

constexpr int kClientAbiVersion = LYFLOW_ABI_VERSION;

class ClientError : public std::runtime_error {
 public:
  explicit ClientError(const std::string& what) : std::runtime_error(what) {}
};

struct InputCloud {
  std::string nodeId;
  std::string port;
  /// 交错的 x,y,z。其余三个通道要么为空，要么与点数对齐。
  std::vector<float> xyz;
  std::vector<float> intensity;
  std::vector<float> normals;
  std::vector<std::uint8_t> rgb;
};

struct RunOptions {
  std::string runId;
  std::string baseDir;
  std::vector<std::string> targets;
  int maxParallel = 0;
  std::uint64_t cacheBudgetBytes = 0;
  bool preview = false;
  std::uint32_t previewMaxPoints = 0;
  std::uint32_t previewBudgetMs = 0;
  bool noReuse = false;
  std::vector<InputCloud> inputs;
};

struct RunResult {
  std::string runId;
  /// run_finished 的 status："ok" / "error" / "cancelled"。一条事件都没有时是 "error"。
  std::string status;
  /// lyflow_run_outputs 的原始 JSON 文本。
  std::string outputs;
  /// 每条 ExecutionEvent 的原始 JSON 文本，按 seq 顺序。
  std::vector<std::string> events;
  /// run_finished 之前 kind=log 且 level=warn/error 的那些消息。
  std::vector<std::string> diagnostics;
  /// 这次运行在结果仓里的索引挂在它上面。RunResult 一析构，cloud() / save()
  /// 就再也取不到东西 —— 所以想取点云的调用方必须让 RunResult 活着。
  /// 它的析构会调回 core，因此 Client 必须比它活得久。
  std::shared_ptr<void> retain;

  bool ok() const { return status == "ok"; }
};

class Client;

/// 借来的点云缓冲，析构时还给 core。
class CloudView {
 public:
  CloudView() = default;
  ~CloudView() { reset(); }
  CloudView(const CloudView&) = delete;
  CloudView& operator=(const CloudView&) = delete;
  CloudView(CloudView&& other) noexcept { steal(other); }
  CloudView& operator=(CloudView&& other) noexcept {
    if (this != &other) {
      reset();
      steal(other);
    }
    return *this;
  }

  bool valid() const { return view_.handle != nullptr; }
  std::uint32_t pointCount() const { return view_.point_count; }
  std::uint32_t totalPoints() const { return view_.total_points; }
  const float* xyz() const { return view_.xyz; }
  const float* intensity() const {
    return (view_.flags & LYFLOW_CLOUD_HAS_INTENSITY) ? view_.intensity : nullptr;
  }
  const float* normals() const {
    return (view_.flags & LYFLOW_CLOUD_HAS_NORMALS) ? view_.normals : nullptr;
  }
  const float* bounds() const { return view_.bounds; }

 private:
  friend class Client;
  void reset();
  void steal(CloudView& other) {
    view_ = other.view_;
    free_ = other.free_;
    std::memset(&other.view_, 0, sizeof(other.view_));
    other.free_ = nullptr;
  }

  lyflow_cloud_view view_{};
  void (*free_)(lyflow_cloud_view*) = nullptr;
};

class TensorView {
 public:
  TensorView() = default;
  ~TensorView() { reset(); }
  TensorView(const TensorView&) = delete;
  TensorView& operator=(const TensorView&) = delete;
  TensorView(TensorView&& other) noexcept { steal(other); }
  TensorView& operator=(TensorView&& other) noexcept {
    if (this != &other) {
      reset();
      steal(other);
    }
    return *this;
  }

  bool valid() const { return view_.handle != nullptr; }
  std::uint32_t rank() const { return view_.rank; }
  std::uint32_t count() const { return view_.count; }
  std::uint64_t offset() const { return view_.offset; }
  std::uint64_t total() const { return view_.total; }
  const std::int64_t* shape() const { return view_.shape; }
  const float* data() const { return view_.data; }

 private:
  friend class Client;
  void reset();
  void steal(TensorView& other) {
    view_ = other.view_;
    free_ = other.free_;
    std::memset(&other.view_, 0, sizeof(other.view_));
    other.free_ = nullptr;
  }

  lyflow_tensor_view view_{};
  void (*free_)(lyflow_tensor_view*) = nullptr;
};

class IndicesView {
 public:
  IndicesView() = default;
  ~IndicesView() { reset(); }
  IndicesView(const IndicesView&) = delete;
  IndicesView& operator=(const IndicesView&) = delete;
  IndicesView(IndicesView&& other) noexcept { steal(other); }
  IndicesView& operator=(IndicesView&& other) noexcept {
    if (this != &other) {
      reset();
      steal(other);
    }
    return *this;
  }

  bool valid() const { return view_.handle != nullptr; }
  std::uint32_t count() const { return view_.count; }
  std::uint32_t total() const { return view_.total; }
  std::uint64_t sourceCloudId() const { return view_.source_cloud_id; }
  const std::int32_t* values() const { return view_.values; }

 private:
  friend class Client;
  void reset();
  void steal(IndicesView& other) {
    view_ = other.view_;
    free_ = other.free_;
    std::memset(&other.view_, 0, sizeof(other.view_));
    other.free_ = nullptr;
  }

  lyflow_indices_view view_{};
  void (*free_)(lyflow_indices_view*) = nullptr;
};

namespace detail {

struct EventSink {
  const std::function<void(const char*)>* fn = nullptr;
};

inline void dispatchEvent(const char* eventJson, void* user) {
  auto* sink = static_cast<EventSink*>(user);
  if (sink && sink->fn && eventJson) (*sink->fn)(eventJson);
}

struct RunState {
  mutable std::mutex mutex;
  std::string status = "error";
  std::vector<std::string> diagnostics;
  std::function<void(const char*)> onEvent;
  std::function<void(const char*)> tap;
  EventSink sink;

  void observe(const char* eventJson) {
    const std::string text(eventJson);
    if (text.find("\"kind\":\"run_finished\"") != std::string::npos) {
      const std::size_t at = text.find("\"status\":\"");
      if (at != std::string::npos) {
        const std::size_t begin = at + 10;
        const std::size_t end = text.find('"', begin);
        if (end != std::string::npos) {
          std::lock_guard<std::mutex> lock(mutex);
          status = text.substr(begin, end - begin);
        }
      }
    } else if (text.find("\"kind\":\"log\"") != std::string::npos &&
               (text.find("\"level\":\"warn\"") != std::string::npos ||
                text.find("\"level\":\"error\"") != std::string::npos)) {
      std::lock_guard<std::mutex> lock(mutex);
      diagnostics.push_back(text);
    }
    if (onEvent) onEvent(eventJson);
  }
};

}  // namespace detail

class RunHandle {
 public:
  RunHandle() = default;
  ~RunHandle() { join(); }

  RunHandle(const RunHandle&) = delete;
  RunHandle& operator=(const RunHandle&) = delete;
  RunHandle(RunHandle&& other) noexcept { steal(other); }
  RunHandle& operator=(RunHandle&& other) noexcept {
    if (this != &other) {
      join();
      reset();
      steal(other);
    }
    return *this;
  }

  bool valid() const { return static_cast<bool>(run_); }
  const std::string& runId() const { return runId_; }
  bool joined() const { return joined_; }

  void cancel() const;
  void join();

  std::string status() const;
  std::vector<std::string> diagnostics() const;

  std::string outputs() const;

  CloudView cloud(const std::string& nodeId, const std::string& port,
                  std::uint32_t maxPoints = 0) const;

  TensorView tensor(const std::string& nodeId, const std::string& port,
                    std::uint64_t offset = 0, std::uint32_t count = 0) const;

  IndicesView indices(const std::string& nodeId, const std::string& port,
                      std::uint64_t offset = 0, std::uint32_t count = 0) const;

  RunResult result();

 private:
  friend class Client;

  void reset() {
    state_.reset();
    run_.reset();
    joined_ = false;
  }

  void steal(RunHandle& other) {
    client_ = other.client_;
    runId_ = std::move(other.runId_);
    state_ = std::move(other.state_);
    run_ = std::move(other.run_);
    joined_ = other.joined_;
    other.client_ = nullptr;
    other.joined_ = true;
  }

  const Client* client_ = nullptr;
  std::string runId_;
  std::shared_ptr<detail::RunState> state_;
  std::shared_ptr<void> run_;
  bool joined_ = false;
};

class Client {
 public:
  explicit Client(const std::string& dllPath) { load(dllPath); }
#if defined(_WIN32)
  explicit Client(const std::wstring& dllPath) { loadW(dllPath); }
#endif

  ~Client() {
    if (handle_) {
#if defined(_WIN32)
      ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
      ::dlclose(handle_);
#endif
    }
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  std::string version() const { return fn_.version(); }

  std::string manifest() const { return owned(fn_.manifest_json()); }

  /// 注册表自检。空字符串 = 干净。宿主启动时应当查一次。
  std::string problems() const { return owned(fn_.manifest_problems()); }

  /// 只校验不执行。返回诊断 JSON 数组文本（"[]" = 干净）。
  std::string validate(const std::string& graphJson, const std::string& baseDir = {}) const {
    return owned(fn_.validate(graphJson.c_str(), baseDir.c_str()));
  }

  std::string plan(const std::string& graphJson, const std::string& baseDir = {},
                   const std::vector<std::string>& targets = {}) const {
    std::vector<const char*> ptrs;
    for (const std::string& t : targets) ptrs.push_back(t.c_str());
    return owned(fn_.plan(graphJson.c_str(), baseDir.c_str(),
                          ptrs.empty() ? nullptr : ptrs.data(), ptrs.size()));
  }

  /// 文本 → 图。成功返回 GraphDoc 对象文本（'{' 开头），失败返回诊断数组（'[' 开头）。
  std::string import(const std::string& kind, const std::string& text,
                     const std::string& baseDir = {}) const {
    return owned(fn_.import(kind.c_str(), text.c_str(), baseDir.c_str()));
  }

  /// 同步跑一张图。回调版是 runAsync，不阻塞的是 startRun。
  RunResult run(const std::string& graphJson, const RunOptions& options = {}) const {
    std::vector<std::string> events;
    RunHandle handle = startRun(graphJson, options, [&events](const char* eventJson) {
      events.emplace_back(eventJson);
    });
    RunResult result = handle.result();
    result.events = std::move(events);
    return result;
  }

  /// 回调版。onEvent 在 core 的工作线程上被调用，本函数返回时保证不会再被调用。
  RunResult runAsync(const std::string& graphJson, const RunOptions& options,
                     const std::function<void(const char*)>& onEvent) const {
    RunHandle handle = startRun(graphJson, options, onEvent);
    return handle.result();
  }

  RunHandle startRun(const std::string& graphJson, const RunOptions& options,
                     const std::function<void(const char*)>& onEvent = {}) const;

  /// 某个节点某个输出端口的点云，等步长抽样到 maxPoints 以内。
  CloudView cloud(const std::string& runId, const std::string& nodeId, const std::string& port,
                  std::uint32_t maxPoints = 0) const {
    CloudView out;
    const int rc = fn_.output_cloud(runId.c_str(), nodeId.c_str(), port.c_str(), maxPoints,
                                    &out.view_);
    if (rc != 0) {
      std::memset(&out.view_, 0, sizeof(out.view_));
      return out;
    }
    out.free_ = fn_.cloud_view_free;
    return out;
  }

  TensorView tensor(const std::string& runId, const std::string& nodeId, const std::string& port,
                    std::uint64_t offset = 0, std::uint32_t count = 0) const {
    TensorView out;
    const int rc = fn_.output_tensor(runId.c_str(), nodeId.c_str(), port.c_str(), offset, count,
                                     &out.view_);
    if (rc != 0) {
      std::memset(&out.view_, 0, sizeof(out.view_));
      return out;
    }
    out.free_ = fn_.tensor_view_free;
    return out;
  }

  IndicesView indices(const std::string& runId, const std::string& nodeId, const std::string& port,
                      std::uint64_t offset = 0, std::uint32_t count = 0) const {
    IndicesView out;
    const int rc = fn_.output_indices(runId.c_str(), nodeId.c_str(), port.c_str(), offset, count,
                                      &out.view_);
    if (rc != 0) {
      std::memset(&out.view_, 0, sizeof(out.view_));
      return out;
    }
    out.free_ = fn_.indices_view_free;
    return out;
  }

  /// 某个节点全部输出端口的元信息 JSON 数组；非点云的项带 value。
  std::string outputInfo(const std::string& runId, const std::string& nodeId) const {
    return owned(fn_.output_info(runId.c_str(), nodeId.c_str()));
  }

  /// 把某个输出整份写盘（PCD/PLY 按扩展名）。失败时抛 ClientError。
  void save(const std::string& runId, const std::string& nodeId, const std::string& port,
            const std::string& path, const std::string& format = {}) const {
    const std::string message =
        owned(fn_.output_save(runId.c_str(), nodeId.c_str(), port.c_str(), path.c_str(),
                              format.c_str()));
    if (!message.empty()) throw ClientError(message);
  }

  void clearCache() const { fn_.cache_clear(); }
  std::string cacheStats() const { return owned(fn_.cache_stats()); }

 private:
  friend class RunHandle;

  struct Table {
    const char* (*version)() = nullptr;
    char* (*manifest_json)() = nullptr;
    char* (*manifest_problems)() = nullptr;
    void (*string_free)(char*) = nullptr;
    char* (*validate)(const char*, const char*) = nullptr;
    char* (*plan)(const char*, const char*, const char* const*, size_t) = nullptr;
    void (*cache_clear)() = nullptr;
    char* (*cache_stats)() = nullptr;
    lyflow_run* (*run_start)(const char*, const lyflow_run_options*, lyflow_event_cb,
                             void*) = nullptr;
    void (*run_cancel)(lyflow_run*) = nullptr;
    void (*run_join)(lyflow_run*) = nullptr;
    void (*run_free)(lyflow_run*) = nullptr;
    char* (*run_outputs)(const char*) = nullptr;
    char* (*import)(const char*, const char*, const char*) = nullptr;
    int (*output_cloud)(const char*, const char*, const char*, uint32_t,
                        lyflow_cloud_view*) = nullptr;
    void (*cloud_view_free)(lyflow_cloud_view*) = nullptr;
    int (*output_tensor)(const char*, const char*, const char*, uint64_t, uint32_t,
                         lyflow_tensor_view*) = nullptr;
    void (*tensor_view_free)(lyflow_tensor_view*) = nullptr;
    int (*output_indices)(const char*, const char*, const char*, uint64_t, uint32_t,
                          lyflow_indices_view*) = nullptr;
    void (*indices_view_free)(lyflow_indices_view*) = nullptr;
    char* (*output_info)(const char*, const char*) = nullptr;
    char* (*output_save)(const char*, const char*, const char*, const char*,
                         const char*) = nullptr;
  };

  void load(const std::string& dllPath);
#if defined(_WIN32)
  void loadW(const std::wstring& dllPath);
#endif
  void bind(const std::string& where);

  void* symbol(const char* name) const {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return ::dlsym(handle_, name);
#endif
  }

  template <typename T>
  void need(T& slot, const char* name, const std::string& where) {
    slot = reinterpret_cast<T>(symbol(name));
    if (!slot) {
      throw ClientError(std::string("lyflow_core 里找不到符号 ") + name + "（" + where +
                        " 的 ABI 与本头文件的 v" + std::to_string(kClientAbiVersion) + " 不匹配）");
    }
  }

  std::string owned(char* raw) const {
    if (!raw) return {};
    std::string out(raw);
    fn_.string_free(raw);
    return out;
  }

  void* handle_ = nullptr;
  Table fn_;
};

// ------------------------------------------------------------------ 实现

inline void CloudView::reset() {
  if (free_ && view_.handle) free_(&view_);
  std::memset(&view_, 0, sizeof(view_));
  free_ = nullptr;
}

inline void TensorView::reset() {
  if (free_ && view_.handle) free_(&view_);
  std::memset(&view_, 0, sizeof(view_));
  free_ = nullptr;
}

inline void IndicesView::reset() {
  if (free_ && view_.handle) free_(&view_);
  std::memset(&view_, 0, sizeof(view_));
  free_ = nullptr;
}

#if defined(_WIN32)
inline void Client::loadW(const std::wstring& dllPath) {
  handle_ = ::LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle_) {
    throw ClientError("加载 lyflow_core.dll 失败，GetLastError=" +
                      std::to_string(static_cast<unsigned long>(::GetLastError())));
  }
  bind("加载的 DLL");
}

inline void Client::load(const std::string& dllPath) {
  const int wide = ::MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, nullptr, 0);
  if (wide <= 0) throw ClientError("DLL 路径不是合法 UTF-8: " + dllPath);
  std::wstring buffer(static_cast<std::size_t>(wide - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, buffer.data(), wide);
  loadW(buffer);
}
#else
inline void Client::load(const std::string& dllPath) {
  handle_ = ::dlopen(dllPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) throw ClientError(std::string("加载 lyflow core 失败: ") + ::dlerror());
  bind("加载的共享库");
}
#endif

inline void Client::bind(const std::string& where) {
  need(fn_.version, "lyflow_version", where);
  need(fn_.manifest_json, "lyflow_manifest_json", where);
  need(fn_.manifest_problems, "lyflow_manifest_problems", where);
  need(fn_.string_free, "lyflow_string_free", where);
  need(fn_.validate, "lyflow_validate", where);
  need(fn_.plan, "lyflow_plan", where);
  need(fn_.cache_clear, "lyflow_cache_clear", where);
  need(fn_.cache_stats, "lyflow_cache_stats", where);
  need(fn_.run_start, "lyflow_run_start", where);
  need(fn_.run_cancel, "lyflow_run_cancel", where);
  need(fn_.run_join, "lyflow_run_join", where);
  need(fn_.run_free, "lyflow_run_free", where);
  need(fn_.run_outputs, "lyflow_run_outputs", where);
  need(fn_.import, "lyflow_import", where);
  need(fn_.output_cloud, "lyflow_output_cloud", where);
  need(fn_.cloud_view_free, "lyflow_cloud_view_free", where);
  need(fn_.output_tensor, "lyflow_output_tensor", where);
  need(fn_.tensor_view_free, "lyflow_tensor_view_free", where);
  need(fn_.output_indices, "lyflow_output_indices", where);
  need(fn_.indices_view_free, "lyflow_indices_view_free", where);
  need(fn_.output_info, "lyflow_output_info", where);
  need(fn_.output_save, "lyflow_output_save", where);
}

inline void RunHandle::cancel() const {
  if (run_ && client_) client_->fn_.run_cancel(static_cast<lyflow_run*>(run_.get()));
}

inline void RunHandle::join() {
  if (joined_ || !run_ || !client_) return;
  client_->fn_.run_join(static_cast<lyflow_run*>(run_.get()));
  joined_ = true;
}

inline std::string RunHandle::status() const {
  if (!state_) return std::string("error");
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->status;
}

inline std::vector<std::string> RunHandle::diagnostics() const {
  if (!state_) return {};
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->diagnostics;
}

inline std::string RunHandle::outputs() const {
  if (!client_) return {};
  return client_->owned(client_->fn_.run_outputs(runId_.c_str()));
}

inline CloudView RunHandle::cloud(const std::string& nodeId, const std::string& port,
                                  std::uint32_t maxPoints) const {
  if (!client_) return CloudView();
  return client_->cloud(runId_, nodeId, port, maxPoints);
}

inline TensorView RunHandle::tensor(const std::string& nodeId, const std::string& port,
                                    std::uint64_t offset, std::uint32_t count) const {
  if (!client_) return TensorView();
  return client_->tensor(runId_, nodeId, port, offset, count);
}

inline IndicesView RunHandle::indices(const std::string& nodeId, const std::string& port,
                                      std::uint64_t offset, std::uint32_t count) const {
  if (!client_) return IndicesView();
  return client_->indices(runId_, nodeId, port, offset, count);
}

inline RunResult RunHandle::result() {
  join();
  RunResult out;
  out.runId = runId_;
  out.retain = run_;
  if (state_) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    out.status = state_->status;
    out.diagnostics = state_->diagnostics;
  }
  out.outputs = outputs();
  return out;
}

inline RunHandle Client::startRun(const std::string& graphJson, const RunOptions& options,
                                  const std::function<void(const char*)>& onEvent) const {
  RunHandle handle;
  handle.client_ = this;
  handle.runId_ = options.runId.empty() ? std::string("embed-run") : options.runId;
  handle.state_ = std::make_shared<detail::RunState>();

  detail::RunState* state = handle.state_.get();
  state->onEvent = onEvent;
  state->tap = [state](const char* eventJson) { state->observe(eventJson); };
  state->sink.fn = &state->tap;

  std::vector<const char*> targets;
  for (const std::string& t : options.targets) targets.push_back(t.c_str());

  std::vector<lyflow_run_input> inputs;
  inputs.reserve(options.inputs.size());
  for (const InputCloud& in : options.inputs) {
    lyflow_run_input raw{};
    raw.node_id = in.nodeId.c_str();
    raw.port = in.port.c_str();
    raw.kind = LYFLOW_INPUT_POINT_CLOUD;
    raw.count = static_cast<std::uint32_t>(in.xyz.size() / 3);
    raw.xyz = in.xyz.empty() ? nullptr : in.xyz.data();
    raw.intensity = in.intensity.empty() ? nullptr : in.intensity.data();
    raw.normals = in.normals.empty() ? nullptr : in.normals.data();
    raw.rgb = in.rgb.empty() ? nullptr : in.rgb.data();
    inputs.push_back(raw);
  }

  lyflow_run_options opts{};
  opts.run_id = handle.runId_.c_str();
  opts.base_dir = options.baseDir.c_str();
  opts.targets = targets.empty() ? nullptr : targets.data();
  opts.target_count = targets.size();
  opts.max_parallel = options.maxParallel;
  opts.cache_budget_bytes = options.cacheBudgetBytes;
  opts.mode = options.preview ? LYFLOW_RUN_MODE_PREVIEW : LYFLOW_RUN_MODE_FULL;
  opts.preview_max_points = options.previewMaxPoints;
  opts.preview_budget_ms = options.previewBudgetMs;
  opts.no_reuse = options.noReuse ? 1 : 0;
  opts.inputs = inputs.empty() ? nullptr : inputs.data();
  opts.input_count = inputs.size();

  lyflow_run* run =
      fn_.run_start(graphJson.c_str(), &opts, &detail::dispatchEvent, &state->sink);
  if (!run) throw ClientError("lyflow_run_start 返回空句柄（内存不足）");

  auto freeRun = fn_.run_free;
  handle.run_ = std::shared_ptr<void>(run, [freeRun](void* p) {
    if (p) freeRun(static_cast<lyflow_run*>(p));
  });
  return handle;
}

}  // namespace lyflow

#endif  // LYFLOW_CLIENT_HPP
