#ifndef LYFLOW_FRAMES_HPP
#define LYFLOW_FRAMES_HPP

#include "lyflow/client.hpp"

namespace lyflow {
namespace detail {
inline int32_t frameCancelled(void* user) noexcept {
  try {
    return (*static_cast<const std::function<bool()>*>(user))() ? 1 : 0;
  } catch (...) {
    return 1;
  }
}
template <typename T> inline T frameRequired(T function, const char* name) {
  if (!function)
    throw ClientError(std::string("运行库缺少可选能力 ") + name);
  return function;
}

struct FrameApi {
  std::shared_ptr<void> lease;
  decltype(&lyflow_frame_capture) capture = nullptr;
  decltype(&lyflow_frame_load) load = nullptr;
  decltype(&lyflow_frame_save) save = nullptr;
  decltype(&lyflow_frame_load_ex) loadEx = nullptr;
  decltype(&lyflow_frame_save_ex) saveEx = nullptr;
  decltype(&lyflow_session_push_ex) pushEx = nullptr;
  decltype(&lyflow_frame_metadata) metadata = nullptr;
  decltype(&lyflow_frame_run_id) runId = nullptr;
  decltype(&lyflow_frame_free) free = nullptr;
  decltype(&lyflow_session_create) create = nullptr;
  decltype(&lyflow_session_push) push = nullptr;
  decltype(&lyflow_session_front) front = nullptr;
  decltype(&lyflow_session_reset) reset = nullptr;
  decltype(&lyflow_session_info) info = nullptr;
  decltype(&lyflow_session_free) sessionFree = nullptr;
  decltype(&lyflow_run_start_frame) start = nullptr;
  decltype(&lyflow_runtime_capabilities) capabilities = nullptr;
  decltype(&lyflow_run_bindings) bindings = nullptr;
  decltype(&lyflow_effective_params_inputs) effectiveInputs = nullptr;
  decltype(&lyflow_string_free) stringFree = nullptr;
  decltype(&lyflow_output_cloud) cloud = nullptr;
  decltype(&lyflow_cloud_view_free) cloudFree = nullptr;
  decltype(&lyflow_output_tensor) tensor = nullptr;
  decltype(&lyflow_tensor_view_free) tensorFree = nullptr;
  decltype(&lyflow_output_image) image = nullptr;
  decltype(&lyflow_image_view_free) imageFree = nullptr;
  decltype(&lyflow_output_indices) indices = nullptr;
  decltype(&lyflow_indices_view_free) indicesFree = nullptr;
  decltype(&lyflow_output_info) outputInfo = nullptr;
  decltype(&lyflow_run_outputs) outputs = nullptr;

  std::string owned(char* raw) const {
    if (!raw)
      throw ClientError("core返回空字符串指针");
    std::unique_ptr<char, decltype(stringFree)> text(raw, stringFree);
    return std::string(text.get());
  }
  void status(char* raw) const {
    const auto message = owned(raw);
    if (!message.empty())
      throw ClientError(message);
  }
  void frameLifetime() const {
    frameRequired(free, "lyflow_frame_free");
    frameRequired(runId, "lyflow_frame_run_id");
  }
};
} // namespace detail

class Frame {
public:
  Frame() = default;
  bool valid() const { return static_cast<bool>(handle_); }
  const std::string& runId() const {
    check();
    return runId_;
  }
  std::string metadata() const {
    check();
    return api_->owned(
        detail::frameRequired(api_->metadata, "lyflow_frame_metadata")(handle_.get()));
  }
  void save(const std::string& path, bool overwrite = false,
            const std::function<bool()>& cancelled = {}) const {
    check();
    if (cancelled) {
      api_->status(detail::frameRequired(api_->saveEx, "lyflow_frame_save_ex")(
          handle_.get(), path.c_str(), overwrite ? 1 : 0, &detail::frameCancelled,
          const_cast<std::function<bool()>*>(&cancelled)));
      return;
    }
    api_->status(detail::frameRequired(api_->save, "lyflow_frame_save")(handle_.get(), path.c_str(),
                                                                        overwrite ? 1 : 0));
  }
  std::string outputs() const {
    check();
    return api_->owned(api_->outputs(runId_.c_str()));
  }
  std::string outputInfo(const std::string& nodeId) const {
    check();
    return api_->owned(api_->outputInfo(runId_.c_str(), nodeId.c_str()));
  }
  CloudView cloud(const std::string& nodeId, const std::string& port,
                  std::uint32_t maximum = 0) const {
    check();
    CloudView view;
    if (api_->cloud(runId_.c_str(), nodeId.c_str(), port.c_str(), maximum, &view.view_) == 0) {
      view.free_ = api_->cloudFree;
      view.retain_ = handle_;
    }
    return view;
  }
  TensorView tensor(const std::string& nodeId, const std::string& port, std::uint64_t offset = 0,
                    std::uint32_t count = 0) const {
    check();
    TensorView view;
    if (api_->tensor(runId_.c_str(), nodeId.c_str(), port.c_str(), offset, count, &view.view_) ==
        0) {
      view.free_ = api_->tensorFree;
      view.retain_ = handle_;
    }
    return view;
  }
  ImageView image(const std::string& nodeId, const std::string& port, std::uint32_t level = 0,
                  std::uint32_t rowOffset = 0, std::uint32_t rowCount = 0) const {
    check();
    ImageView view;
    if (api_->image(runId_.c_str(), nodeId.c_str(), port.c_str(), level, rowOffset, rowCount,
                    &view.view_) == 0) {
      view.free_ = api_->imageFree;
      view.retain_ = handle_;
    }
    return view;
  }
  IndicesView indices(const std::string& nodeId, const std::string& port, std::uint64_t offset = 0,
                      std::uint32_t count = 0) const {
    check();
    IndicesView view;
    if (api_->indices(runId_.c_str(), nodeId.c_str(), port.c_str(), offset, count, &view.view_) ==
        0) {
      view.free_ = api_->indicesFree;
      view.retain_ = handle_;
    }
    return view;
  }

private:
  friend class Client;
  friend class FrameSession;
  Frame(std::shared_ptr<detail::FrameApi> api, lyflow_frame* raw) : api_(std::move(api)) {
    if (!raw)
      throw ClientError("core返回空Frame");
    handle_ =
        std::shared_ptr<lyflow_frame>(raw, [api = api_](lyflow_frame* frame) { api->free(frame); });
    const char* id = api_->runId(raw);
    if (!id)
      throw ClientError("core返回空Frame viewId");
    runId_ = id;
  }
  void check() const {
    if (!handle_)
      throw ClientError("无效Frame句柄");
  }
  std::shared_ptr<detail::FrameApi> api_;
  std::shared_ptr<lyflow_frame> handle_;
  std::string runId_;
};

class FrameSession {
public:
  FrameSession() = default;
  bool valid() const { return static_cast<bool>(handle_); }
  std::string push(const Frame& frame, bool reset = false,
                   const std::function<bool()>& cancelled = {}) const {
    check();
    frame.check();
    if (api_->lease.get() != frame.api_->lease.get())
      throw ClientError("Frame来自不同运行库");
    if (cancelled)
      return api_->owned(detail::frameRequired(api_->pushEx, "lyflow_session_push_ex")(
          handle_.get(), frame.handle_.get(), reset ? 1 : 0, &detail::frameCancelled,
          const_cast<std::function<bool()>*>(&cancelled)));
    return api_->owned(detail::frameRequired(api_->push, "lyflow_session_push")(
        handle_.get(), frame.handle_.get(), reset ? 1 : 0));
  }
  Frame front(bool remove = false) const {
    check();
    api_->frameLifetime();
    lyflow_frame* raw = nullptr;
    api_->status(detail::frameRequired(api_->front, "lyflow_session_front")(handle_.get(),
                                                                            remove ? 1 : 0, &raw));
    return raw ? Frame(api_, raw) : Frame();
  }
  void reset() const {
    check();
    api_->status(detail::frameRequired(api_->reset, "lyflow_session_reset")(handle_.get()));
  }
  std::string info() const {
    check();
    return api_->owned(detail::frameRequired(api_->info, "lyflow_session_info")(handle_.get()));
  }

private:
  friend class Client;
  FrameSession(std::shared_ptr<detail::FrameApi> api, lyflow_session* raw) : api_(std::move(api)) {
    if (!raw)
      throw ClientError("core返回空Session");
    handle_ = std::shared_ptr<lyflow_session>(
        raw, [api = api_](lyflow_session* session) { api->sessionFree(session); });
  }
  void check() const {
    if (!handle_)
      throw ClientError("无效Session句柄");
  }
  std::shared_ptr<detail::FrameApi> api_;
  std::shared_ptr<lyflow_session> handle_;
};

inline std::shared_ptr<detail::FrameApi> Client::frameApi() const {
  auto api = std::make_shared<detail::FrameApi>();
  api->lease = lease_;
#define LYFLOW_FRAME_BIND(field, name)                                                             \
  api->field = reinterpret_cast<decltype(api->field)>(symbol(name))
  LYFLOW_FRAME_BIND(capture, "lyflow_frame_capture");
  LYFLOW_FRAME_BIND(load, "lyflow_frame_load");
  LYFLOW_FRAME_BIND(save, "lyflow_frame_save");
  LYFLOW_FRAME_BIND(loadEx, "lyflow_frame_load_ex");
  LYFLOW_FRAME_BIND(saveEx, "lyflow_frame_save_ex");
  LYFLOW_FRAME_BIND(pushEx, "lyflow_session_push_ex");
  LYFLOW_FRAME_BIND(metadata, "lyflow_frame_metadata");
  LYFLOW_FRAME_BIND(runId, "lyflow_frame_run_id");
  LYFLOW_FRAME_BIND(free, "lyflow_frame_free");
  LYFLOW_FRAME_BIND(create, "lyflow_session_create");
  LYFLOW_FRAME_BIND(push, "lyflow_session_push");
  LYFLOW_FRAME_BIND(front, "lyflow_session_front");
  LYFLOW_FRAME_BIND(reset, "lyflow_session_reset");
  LYFLOW_FRAME_BIND(info, "lyflow_session_info");
  LYFLOW_FRAME_BIND(sessionFree, "lyflow_session_free");
  LYFLOW_FRAME_BIND(start, "lyflow_run_start_frame");
  LYFLOW_FRAME_BIND(capabilities, "lyflow_runtime_capabilities");
  LYFLOW_FRAME_BIND(bindings, "lyflow_run_bindings");
  LYFLOW_FRAME_BIND(effectiveInputs, "lyflow_effective_params_inputs");
#undef LYFLOW_FRAME_BIND
  api->stringFree = fn_.string_free;
  api->cloud = fn_.output_cloud;
  api->cloudFree = fn_.cloud_view_free;
  api->tensor = fn_.output_tensor;
  api->tensorFree = fn_.tensor_view_free;
  api->image = fn_.output_image;
  api->imageFree = fn_.image_view_free;
  api->indices = fn_.output_indices;
  api->indicesFree = fn_.indices_view_free;
  api->outputInfo = fn_.output_info;
  api->outputs = fn_.run_outputs;
  return api;
}

inline std::string Client::runtimeCapabilities() const {
  const auto api = frameApi();
  return api->owned(detail::frameRequired(api->capabilities, "lyflow_runtime_capabilities")());
}
inline std::string Client::runBindings(const std::string& runId) const {
  const auto api = frameApi();
  return api->owned(detail::frameRequired(api->bindings, "lyflow_run_bindings")(runId.c_str()));
}
inline std::string Client::effectiveParams(const std::string& graphJson, const std::string& baseDir,
                                           const std::string& paramsJson,
                                           const std::vector<InputCloud>& inputs,
                                           const std::vector<InputImage>& imageInputs) const {
  const auto api = frameApi();
  const auto call = detail::frameRequired(api->effectiveInputs, "lyflow_effective_params_inputs");
  const detail::RawInputs raw(inputs, imageInputs);
  return api->owned(call(graphJson.c_str(), baseDir.c_str(),
                         paramsJson.empty() ? nullptr : paramsJson.c_str(), raw.clouds(),
                         raw.cloudCount(), raw.images(), raw.imageCount()));
}
inline Frame Client::captureFrame(const std::string& runId, const std::string& metadataJson,
                                  const std::string& optionsJson) const {
  const auto api = frameApi();
  const auto call = detail::frameRequired(api->capture, "lyflow_frame_capture");
  api->frameLifetime();
  lyflow_frame* raw = nullptr;
  api->status(call(runId.c_str(), metadataJson.c_str(), optionsJson.c_str(), &raw));
  return Frame(api, raw);
}
inline Frame Client::loadFrame(const std::string& path, std::uint64_t maxBytes,
                               const std::function<bool()>& cancelled) const {
  const auto api = frameApi();
  api->frameLifetime();
  lyflow_frame* raw = nullptr;
  if (cancelled)
    api->status(detail::frameRequired(api->loadEx, "lyflow_frame_load_ex")(
        path.c_str(), maxBytes, &detail::frameCancelled,
        const_cast<std::function<bool()>*>(&cancelled), &raw));
  else
    api->status(
        detail::frameRequired(api->load, "lyflow_frame_load")(path.c_str(), maxBytes, &raw));
  return Frame(api, raw);
}
inline FrameSession Client::createSession(const std::string& optionsJson) const {
  const auto api = frameApi();
  const auto call = detail::frameRequired(api->create, "lyflow_session_create");
  detail::frameRequired(api->sessionFree, "lyflow_session_free");
  lyflow_session* raw = nullptr;
  api->status(call(optionsJson.c_str(), &raw));
  return FrameSession(api, raw);
}
inline lyflow_run* Client::startFrame(const char* graph, const lyflow_run_options* options,
                                      const Frame* frame, const std::string* bindings,
                                      lyflow_event_cb callback, void* user,
                                      std::shared_ptr<void>& retain) const {
  frame->check();
  if (lease_.get() != frame->api_->lease.get())
    throw ClientError("Frame来自不同运行库");
  const auto call = detail::frameRequired(frame->api_->start, "lyflow_run_start_frame");
  retain = frame->handle_;
  return call(graph, options, frame->handle_.get(), bindings->c_str(), callback, user);
}
inline RunHandle Client::startRunWithFrame(const std::string& graphJson, const RunOptions& options,
                                           const Frame& frame, const std::string& bindingsJson,
                                           const std::function<void(const char*)>& onEvent) const {
  return startRunImpl(graphJson, options, onEvent, &frame, &bindingsJson);
}
inline RunResult Client::runWithFrame(const std::string& graphJson, const RunOptions& options,
                                      const Frame& frame, const std::string& bindingsJson) const {
  std::vector<std::string> events;
  std::mutex mutex;
  auto handle = startRunWithFrame(graphJson, options, frame, bindingsJson, [&](const char* event) {
    std::lock_guard<std::mutex> lock(mutex);
    events.emplace_back(event);
  });
  auto result = handle.result();
  result.events = std::move(events);
  return result;
}
} // namespace lyflow

#endif
