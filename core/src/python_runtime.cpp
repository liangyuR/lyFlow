#include "python_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "exec/subgraph.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lyflow::python_runtime {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxFrame = 16 * 1024 * 1024;
constexpr std::size_t kMaxArray = 1024ull * 1024 * 1024;
std::atomic<std::uint64_t> sequence{0};

struct Failure : std::runtime_error {
  std::string code;
  std::string traceback;
  Failure(std::string code, const std::string& message, std::string traceback = {})
      : std::runtime_error(message), code(std::move(code)), traceback(std::move(traceback)) {}
};

struct Directory {
  fs::path path;
  Directory() {
    path = fs::temp_directory_path() /
           ("lyflow-python-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++sequence));
    fs::create_directory(path);
  }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

Json valueJson(const Value& v) {
  switch (v.kind()) {
  case Value::Kind::Bool:
    return v.boolValue();
  case Value::Kind::Int:
    return v.intValue();
  case Value::Kind::Float:
    return v.floatValue();
  case Value::Kind::String:
    return v.stringValue();
  case Value::Kind::FloatVec:
    return v.vecValue();
  default:
    return nullptr;
  }
}

Value scalarValue(const Json& j) {
  if (j.is_boolean())
    return Value::boolean(j.get<bool>());
  if (j.is_number_integer())
    return Value::integer(j.get<std::int64_t>());
  if (j.is_number())
    return Value::number(j.get<double>());
  if (j.is_string())
    return Value::text(j.get<std::string>());
  if (j.is_array())
    return Value::vec(j.get<std::vector<double>>());
  return {};
}

Condition condition(const Json& j) {
  Condition c;
  if (j.is_null())
    return c;
  c.param = j.at("param").get<std::string>();
  if (j.contains("eq"))
    c.eq = scalarValue(j.at("eq"));
  if (j.contains("ne"))
    c.ne = scalarValue(j.at("ne"));
  if (j.contains("in"))
    for (const auto& v : j.at("in"))
      c.in.push_back(scalarValue(v));
  return c;
}

Json writeArray(const fs::path& dir, const void* data, std::size_t bytes,
                const std::vector<std::size_t>& shape, const char* dtype) {
  if (bytes > kMaxArray)
    throw Failure("python_input_invalid", "输入数组超过 1 GiB");
  const fs::path file = dir / (std::to_string(++sequence) + ".bin");
  std::ofstream out(file, std::ios::binary);
  if (bytes)
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  out.close();
  if (!out)
    throw Failure("python_input_invalid", "无法写入 Python 输入数组");
  return {{"file", file.u8string()}, {"shape", shape}, {"dtype", dtype}};
}

Json encode(const Data& data, const fs::path& dir) {
  if (const auto* c = data.asCloud()) {
    if (!c->channelsConsistent())
      throw Failure("python_input_invalid", "点云通道长度不一致");
    const auto n = c->pointCount();
    Json j{{"type", "PointCloud"},
           {"xyz", writeArray(dir, c->xyz.data(), c->xyz.size() * sizeof(float), {n, 3}, "f32")}};
    if (!c->intensity.empty())
      j["intensity"] = writeArray(dir, c->intensity.data(), n * sizeof(float), {n}, "f32");
    if (!c->normals.empty())
      j["normals"] = writeArray(dir, c->normals.data(), n * 3 * sizeof(float), {n, 3}, "f32");
    if (!c->rgb.empty())
      j["rgb"] = writeArray(dir, c->rgb.data(), n * 3, {n, 3}, "u8");
    return j;
  }
  if (const auto* i = data.asImage()) {
    if (!i->consistent())
      throw Failure("python_input_invalid", "图像尺寸或像素缓冲无效");
    return {{"type", "Image"},
            {"pixels",
             writeArray(dir, i->pixels.get(), i->byteSize(),
                        {static_cast<std::size_t>(i->height), static_cast<std::size_t>(i->width),
                         static_cast<std::size_t>(i->channels)},
                        pixelDepthName(i->depth))}};
  }
  if (const auto* m = data.asMeasurement()) {
    Json j{{"type", "Measurement"}, {"value", std::isfinite(m->value) ? Json(m->value) : Json()},
           {"unit", m->unit},       {"ok", m->ok},
           {"message", m->message}, {"verdict", m->verdict}};
    if (m->hasLimits) {
      j["nominal"] = m->nominal;
      j["upper"] = m->upper;
      j["lower"] = m->lower;
    }
    return j;
  }
  if (const auto* r = data.asRecord())
    return {{"type", "Record"}, {"recordType", r->type}, {"data", r->data}};
  throw Failure("python_input_invalid", std::string("Python 不支持端口类型 ") + data.typeName());
}

struct Array {
  fs::path file;
  std::vector<std::size_t> shape;
  std::string dtype;
  std::size_t bytes = 0;
};

Array arrayInfo(const Json& j, const fs::path& dir) {
  Array a;
  a.dtype = j.at("dtype").get<std::string>();
  std::size_t size = a.dtype == "f32" ? 4 : a.dtype == "u16" ? 2 : a.dtype == "u8" ? 1 : 0;
  if (!size || !j.at("shape").is_array() || j.at("shape").empty() || j.at("shape").size() > 3)
    throw Failure("python_output_invalid", "数组 dtype 或 shape 无效");
  for (const auto& d : j.at("shape")) {
    if (!d.is_number_integer() || d.get<std::int64_t>() < 0 ||
        d.get<std::uint64_t>() > std::numeric_limits<std::int32_t>::max())
      throw Failure("python_output_invalid", "数组维度无效");
    const auto n = d.get<std::size_t>();
    if (n && size > kMaxArray / n)
      throw Failure("python_output_invalid", "输出数组超过 1 GiB");
    size *= n;
    a.shape.push_back(n);
  }
  a.bytes = size;
  const fs::path raw = fs::u8path(j.at("file").get<std::string>());
  if (!raw.is_absolute())
    throw Failure("python_output_invalid", "数组文件必须是绝对路径");
  a.file = fs::canonical(raw);
  const fs::path relative = a.file.lexically_relative(fs::canonical(dir));
  if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
      !fs::is_regular_file(a.file) || fs::file_size(a.file) != a.bytes)
    throw Failure("python_output_invalid", "数组文件越界或长度与 shape 不符");
  return a;
}

void readArray(const Array& a, void* target) {
  std::ifstream in(a.file, std::ios::binary);
  if (a.bytes)
    in.read(static_cast<char*>(target), static_cast<std::streamsize>(a.bytes));
  if (!in)
    throw Failure("python_output_invalid", "无法完整读取输出数组");
}

Data decode(const Json& j, const std::string& type, const fs::path& dir) {
  if (!j.is_object() || j.at("type") != type)
    throw Failure("python_output_invalid", "输出 DTO 类型与端口声明不符");
  if (type == "PointCloud") {
    PointCloud c;
    const auto xyz = arrayInfo(j.at("xyz"), dir);
    if (xyz.dtype != "f32" || xyz.shape.size() != 2 || xyz.shape[1] != 3)
      throw Failure("python_output_invalid", "点云 xyz 应为 f32[N,3]");
    const auto n = xyz.shape[0];
    c.xyz.resize(n * 3);
    readArray(xyz, c.xyz.data());
    for (const auto* name : {"intensity", "normals", "rgb"}) {
      if (!j.contains(name))
        continue;
      const auto a = arrayInfo(j.at(name), dir);
      const bool intensity = std::strcmp(name, "intensity") == 0;
      const bool rgb = std::strcmp(name, "rgb") == 0;
      if (a.dtype != (rgb ? "u8" : "f32") ||
          a.shape != (intensity ? std::vector<std::size_t>{n} : std::vector<std::size_t>{n, 3}))
        throw Failure("python_output_invalid", "点云附加通道形状或 dtype 无效");
      if (rgb) {
        c.rgb.resize(n * 3);
        readArray(a, c.rgb.data());
      } else {
        auto& target = intensity ? c.intensity : c.normals;
        target.resize(intensity ? n : n * 3);
        readArray(a, target.data());
      }
    }
    return Data::cloud(std::move(c));
  }
  if (type == "Image") {
    const auto a = arrayInfo(j.at("pixels"), dir);
    if ((a.shape.size() != 2 && a.shape.size() != 3) || !a.shape[0] || !a.shape[1])
      throw Failure("python_output_invalid", "图像应为非空 [H,W] 或 [H,W,C]");
    const auto channels = a.shape.size() == 2 ? 1 : a.shape[2];
    if (channels != 1 && channels != 3 && channels != 4)
      throw Failure("python_output_invalid", "图像通道数应为 1、3 或 4");
    auto image =
        Image::allocate(static_cast<std::int32_t>(a.shape[1]),
                        static_cast<std::int32_t>(a.shape[0]), static_cast<std::int32_t>(channels),
                        a.dtype == "f32"   ? PixelDepth::F32
                        : a.dtype == "u16" ? PixelDepth::U16
                                           : PixelDepth::U8);
    readArray(a, image.mutablePixels());
    return Data::image(std::move(image));
  }
  if (type == "Measurement") {
    Measurement m;
    m.value = j.at("value").is_null() ? std::numeric_limits<double>::quiet_NaN()
                                      : j.at("value").get<double>();
    m.unit = j.value("unit", std::string());
    m.ok = j.at("ok").get<bool>();
    m.message = j.value("message", std::string());
    m.verdict = j.value("verdict", std::string());
    m.hasLimits = j.contains("nominal") || j.contains("upper") || j.contains("lower");
    if (m.hasLimits) {
      m.nominal = j.at("nominal").get<double>();
      m.upper = j.at("upper").get<double>();
      m.lower = j.at("lower").get<double>();
    }
    if (m.ok && !std::isfinite(m.value))
      throw Failure("python_output_invalid", "有效测量值必须是有限数");
    return Data::measurement(std::move(m));
  }
  if (type == "Record")
    return Data::record({j.value("recordType", std::string()), j.at("data")});
  throw Failure("python_output_invalid", "Python 不支持此输出端口类型");
}

#if defined(_WIN32)
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  void reset(HANDLE h = INVALID_HANDLE_VALUE) {
    if (value != INVALID_HANDLE_VALUE && value != nullptr)
      CloseHandle(value);
    value = h;
  }
  bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct PendingConnection {
  HANDLE pipe;
  OVERLAPPED* io;
  bool pending;
  ~PendingConnection() {
    if (pending) {
      CancelIoEx(pipe, io);
      DWORD ignored = 0;
      GetOverlappedResult(pipe, io, &ignored, TRUE);
    }
  }
};

std::wstring wide(const std::string& text) { return fs::u8path(text).wstring(); }

std::wstring quote(const std::wstring& arg) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    result += c;
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

fs::path defaultSdk() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&defaultSdk), &module))
    throw Failure("python_start_failed", "无法定位运行库目录");
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (!n || n == path.size())
    throw Failure("python_start_failed", "无法定位运行库路径");
  path.resize(n);
  return fs::path(path).parent_path() / "python";
}

class Worker {
public:
  ~Worker() { stop(); }
  void stop(ExecContext* ctx = nullptr) {
    if (process_.valid() && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) {
      if (job_.valid())
        TerminateJobObject(job_.value, 1);
      if (WaitForSingleObject(process_.value, 100) == WAIT_TIMEOUT)
        TerminateProcess(process_.value, 1);
      WaitForSingleObject(process_.value, 3000);
    }
    drainLogs(ctx);
    pipe_.reset();
    process_.reset();
    job_.reset();
    log_.reset();
    directory_.reset();
  }
  bool alive() const {
    return process_.valid() && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT;
  }

  void drainLogs(ExecContext* ctx) {
    if (!directory_)
      return;
    std::error_code ec;
    const auto end = fs::file_size(directory_->path / "worker.log", ec);
    if (ec)
      return;
    if (!ctx) {
      logOffset_ = end;
      return;
    }
    constexpr std::uint64_t limit = 1024 * 1024;
    if (end > logOffset_ && end - logOffset_ > limit) {
      logOffset_ = end - limit;
      ctx->log(LogLevel::Warn, "Python stdout/stderr 日志已截断，仅保留最后 1 MiB");
    }
    while (logOffset_ < end) {
      const auto text =
          logs(static_cast<std::size_t>(std::min<std::uint64_t>(65536, end - logOffset_)));
      if (text.empty())
        return;
      ctx->log(LogLevel::Info, text);
    }
  }

  void start(const std::string& executable, const fs::path& sdk, Clock::time_point deadline,
             ExecContext* ctx = nullptr) {
    stop();
    if (ctx && ctx->cancelled())
      throw Failure("python_cancelled", "Python worker 启动已取消");
    logOffset_ = 0;
    directory_ = std::make_unique<Directory>();
    const std::wstring name = L"\\\\.\\pipe\\lyflow-python-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(++sequence);
    pipe_.reset(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
        65536, 0, nullptr));
    if (!pipe_.valid())
      throw Failure("python_start_failed", "无法创建 Python 命名管道");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connection{};
    connection.hEvent = event.value;
    const BOOL connected = ConnectNamedPipe(pipe_.value, &connection);
    const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED)
      throw Failure("python_start_failed", "无法等待 Python 管道连接");
    PendingConnection pending{pipe_.value, &connection, connectError == ERROR_IO_PENDING};
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    log_.reset(CreateFileW((directory_->path / "worker.log").c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log_.valid() || !input.valid())
      throw Failure("python_start_failed", "无法创建 Python 日志通道");
    std::wstring command = quote(wide(executable)) + L" -u -B " +
                           quote((sdk / "lyflow_python" / "worker.py").wstring()) + L" --pipe " +
                           quote(name);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = log_.value;
    startup.hStdError = log_.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
      throw Failure("python_start_failed",
                    "无法启动 Python 解释器，Windows 错误 " + std::to_string(GetLastError()));
    process_.reset(process.hProcess);
    Handle thread(process.hThread);
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_.valid() ||
        !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)) ||
        !AssignProcessToJobObject(job_.value, process_.value)) {
      throw Failure("python_start_failed", "无法隔离 Python 进程树");
    }
    ResumeThread(thread.value);
    if (!connected && connectError != ERROR_PIPE_CONNECTED) {
      while (WaitForSingleObject(event.value, 20) == WAIT_TIMEOUT) {
        if (ctx && ctx->cancelled())
          throw Failure("python_cancelled", "Python worker 连接已取消");
        if (!alive() || Clock::now() >= deadline) {
          CancelIoEx(pipe_.value, &connection);
          WaitForSingleObject(event.value, INFINITE);
          throw Failure("python_start_failed", "Python 未能连接运行库管道" + logDetails(ctx));
        }
      }
      DWORD ignored = 0;
      if (!GetOverlappedResult(pipe_.value, &connection, &ignored, FALSE))
        throw Failure("python_start_failed", "Python 管道连接失败");
      pending.pending = false;
    }
  }

  Json exchange(Json request, Clock::time_point deadline, ExecContext* ctx = nullptr) {
    const auto id = ++sequence;
    request["id"] = id;
    send(request, deadline, ctx);
    std::optional<Clock::time_point> interrupted;
    std::string interruptCode;
    auto check = [&] {
      const auto now = Clock::now();
      if (!interrupted && ((ctx && ctx->cancelled()) || now >= deadline)) {
        interruptCode = ctx && ctx->cancelled() ? "python_cancelled" : "python_timeout";
        interrupted = now;
        send({{"id", id}, {"command", "cancel"}}, now + std::chrono::milliseconds(100), ctx);
      }
      if (interrupted && now - *interrupted >= std::chrono::milliseconds(500)) {
        stop(ctx);
        throw Failure(interruptCode, interruptCode == "python_cancelled" ? "Python 运行已取消"
                                                                         : "Python 运行超时");
      }
      if (!alive())
        throw Failure("python_worker_crashed", "Python worker 意外退出" + logDetails(ctx));
    };
    for (;;) {
      const Json response = receive(check, ctx);
      if (!response.is_object() || response.at("id") != id)
        throw Failure("python_protocol_error", "Python 响应 ID 不匹配");
      const auto event = response.at("event").get<std::string>();
      if (event == "log") {
        if (ctx) {
          const auto level = response.value("level", std::string("info"));
          ctx->log(level == "error"   ? LogLevel::Error
                   : level == "warn"  ? LogLevel::Warn
                   : level == "debug" ? LogLevel::Debug
                                      : LogLevel::Info,
                   response.value("message", std::string()));
        }
      } else if (event == "progress") {
        const auto ratio = response.at("ratio").get<float>();
        if (!std::isfinite(ratio) || ratio < 0 || ratio > 1)
          throw Failure("python_protocol_error", "Python 进度比例无效");
        if (ctx)
          ctx->progress(ratio, response.value("message", std::string()));
      } else if (event == "cancelled") {
        stop(ctx);
        throw Failure("python_cancelled", "Python 运行已取消");
      } else if (event == "error") {
        drainLogs(ctx);
        if (ctx && response.contains("traceback"))
          ctx->log(LogLevel::Error, response.at("traceback").get<std::string>());
        throw Failure(response.value("code", std::string("python_exception")),
                      response.value("message", std::string("Python 执行失败")),
                      response.value("traceback", std::string()));
      } else if (event == "result") {
        if (interrupted) {
          stop(ctx);
          throw Failure(interruptCode, "Python 任务已中止");
        }
        drainLogs(ctx);
        return response;
      } else {
        throw Failure("python_protocol_error", "未知 Python 响应事件");
      }
    }
  }

private:
  Handle pipe_, process_, job_, log_;
  std::unique_ptr<Directory> directory_;
  std::uint64_t logOffset_ = 0;

  std::string logs(std::size_t limit = 65536) {
    if (!directory_)
      return {};
    std::ifstream in(directory_->path / "worker.log", std::ios::binary);
    in.seekg(static_cast<std::streamoff>(logOffset_));
    std::string text(limit, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    logOffset_ += text.size();
    return text;
  }

  std::string logDetails(ExecContext* ctx) {
    if (!ctx)
      return logs();
    drainLogs(ctx);
    return {};
  }

  void send(const Json& j, Clock::time_point deadline, ExecContext* ctx) {
    const auto text = j.dump();
    if (text.size() > kMaxFrame)
      throw Failure("python_protocol_error", "Python 请求帧过大");
    const auto n = static_cast<std::uint32_t>(text.size());
    std::vector<char> frame(4 + text.size());
    for (unsigned i = 0; i < 4; ++i)
      frame[i] = static_cast<char>(n >> (8 * i));
    std::memcpy(frame.data() + 4, text.data(), text.size());
    std::size_t done = 0;
    while (done < frame.size()) {
      Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      OVERLAPPED io{};
      io.hEvent = event.value;
      DWORD wrote = 0;
      const BOOL ok = WriteFile(pipe_.value, frame.data() + done,
                                static_cast<DWORD>(frame.size() - done), &wrote, &io);
      if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING)
          throw Failure("python_worker_crashed", "Python 管道写入失败" + logDetails(ctx));
        while (WaitForSingleObject(event.value, 20) == WAIT_TIMEOUT) {
          if (Clock::now() >= deadline || !alive()) {
            CancelIoEx(pipe_.value, &io);
            WaitForSingleObject(event.value, INFINITE);
            throw Failure("python_timeout", "Python 管道写入超时");
          }
        }
        if (!GetOverlappedResult(pipe_.value, &io, &wrote, FALSE))
          throw Failure("python_worker_crashed", "Python 管道写入中断" + logDetails(ctx));
      }
      if (!wrote)
        throw Failure("python_protocol_error", "Python 管道写入为空");
      done += wrote;
    }
  }

  Json receive(const std::function<void()>& check, ExecContext* ctx) {
    auto read = [&](char* target, std::size_t count) {
      std::size_t done = 0;
      while (done < count) {
        check();
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_.value, nullptr, 0, nullptr, &available, nullptr))
          throw Failure("python_worker_crashed", "Python 管道断开" + logDetails(ctx));
        if (!available) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED io{};
        io.hEvent = event.value;
        DWORD received = 0;
        const auto n = static_cast<DWORD>(std::min<std::size_t>(available, count - done));
        const BOOL ok = ReadFile(pipe_.value, target + done, n, &received, &io);
        if (!ok) {
          if (GetLastError() != ERROR_IO_PENDING)
            throw Failure("python_worker_crashed", "Python 管道读取失败" + logDetails(ctx));
          while (WaitForSingleObject(event.value, 20) == WAIT_TIMEOUT) {
            try {
              check();
            } catch (...) {
              CancelIoEx(pipe_.value, &io);
              WaitForSingleObject(event.value, INFINITE);
              throw;
            }
          }
          if (!GetOverlappedResult(pipe_.value, &io, &received, FALSE))
            throw Failure("python_worker_crashed", "Python 管道读取中断" + logDetails(ctx));
        }
        if (!received)
          throw Failure("python_worker_crashed", "Python 管道意外关闭" + logDetails(ctx));
        done += received;
      }
    };
    unsigned char header[4];
    read(reinterpret_cast<char*>(header), 4);
    std::uint32_t count = 0;
    for (unsigned i = 0; i < 4; ++i)
      count |= static_cast<std::uint32_t>(header[i]) << (8 * i);
    if (!count || count > kMaxFrame)
      throw Failure("python_protocol_error", "Python 响应帧长度无效");
    std::string body(count, '\0');
    read(body.data(), count);
    try {
      return Json::parse(body);
    } catch (const std::exception& e) {
      throw Failure("python_protocol_error", std::string("Python JSON 响应无效: ") + e.what());
    }
  }
};
#else
fs::path defaultSdk() { return {}; }
class Worker {
public:
  void stop(ExecContext* = nullptr) {}
  void drainLogs(ExecContext*) {}
  bool alive() const { return false; }
  void start(const std::string&, const fs::path&, Clock::time_point, ExecContext* = nullptr) {
    throw Failure("python_platform_unsupported", "Python worker 当前需要 Windows 命名管道");
  }
  Json exchange(Json, Clock::time_point, ExecContext* = nullptr) { return {}; }
};
#endif

class Runtime {
public:
  Runtime() { publish(); }
  Json state() const {
    return {{"configured", !executable_.empty()},
            {"ready", ready_},
            {"operatorCount", count_},
            {"problems", problems_},
            {"pythonVersion", pythonVersion_},
            {"numpyVersion", numpyVersion_},
            {"executable", executable_},
            {"packagePaths", paths_},
            {"timeoutMs", timeout_}};
  }
  std::mutex mutex;
  std::timed_mutex tasks;

  Json snapshot() const {
    std::lock_guard<std::mutex> lock(publishedMutex_);
    return published_;
  }

  Json configure(Registry& registry, const std::string& text) {
    std::lock_guard<std::timed_mutex> runLock(tasks);
    std::lock_guard<std::mutex> lock(mutex);
    worker_.stop();
    ready_ = false;
    count_ = 0;
    problems_ = Json::array();
    pythonVersion_.clear();
    numpyVersion_.clear();
    executable_.clear();
    paths_.clear();
    timeout_ = 60000;
    sdk_.clear();
    publish();
    try {
      const Json config = text.empty() ? Json::object() : Json::parse(text);
      if (!config.is_object())
        throw Failure("python_config_invalid", "Python 配置必须是 JSON 对象");
      executable_ = config.value("executable", std::string());
      paths_ = config.value("packagePaths", std::vector<std::string>{});
      timeout_ = config.value("timeoutMs", 60000);
      if (timeout_ < 1 || timeout_ > 24 * 60 * 60 * 1000)
        throw Failure("python_config_invalid", "timeoutMs 必须在 1 到 86400000 之间");
      sdk_ = config.contains("sdkDir")
                 ? fs::absolute(fs::u8path(config.at("sdkDir").get<std::string>()))
                 : defaultSdk();
      for (auto& path : paths_)
        path = fs::absolute(fs::u8path(path)).u8string();
      return discover(registry);
    } catch (const std::exception& e) {
      worker_.stop();
      problem(e.what());
      commit(registry, {});
      return publish();
    }
  }

  Json refresh(Registry& registry) {
    std::lock_guard<std::timed_mutex> runLock(tasks);
    std::lock_guard<std::mutex> lock(mutex);
    worker_.stop();
    return discover(registry);
  }

private:
  Worker worker_;
  Json workerDefinitions_ = Json::object();
  std::string executable_, pythonVersion_, numpyVersion_;
  std::vector<std::string> paths_;
  fs::path sdk_;
  int timeout_ = 60000;
  bool ready_ = false;
  std::size_t count_ = 0;
  Json problems_ = Json::array();
  mutable std::mutex publishedMutex_;
  Json published_;

  Json publish() {
    const auto value = state();
    std::lock_guard<std::mutex> lock(publishedMutex_);
    published_ = value;
    return value;
  }

  void problem(const std::string& message, const std::string& path = {},
               const std::string& traceback = {}) {
    Json item{{"path", path}, {"message", message}};
    if (!traceback.empty())
      item["traceback"] = traceback;
    problems_.push_back(std::move(item));
  }

  void commit(Registry& registry, std::vector<OperatorDesc> ops) {
    std::unique_lock<std::shared_mutex> lock(registry.mutex());
    registry.setPythonOperators(std::move(ops));
  }

  Json launch(Clock::time_point deadline, ExecContext* ctx = nullptr) {
    workerDefinitions_ = Json::object();
    worker_.start(executable_, sdk_, deadline, ctx);
    auto result =
        worker_.exchange({{"command", "discover"}, {"packagePaths", paths_}}, deadline, ctx);
    for (const auto& definition : result.at("operators"))
      workerDefinitions_[definition.at("id").get<std::string>()] = definition;
    return result;
  }

  Json discover(Registry& registry) {
    ready_ = false;
    count_ = 0;
    problems_ = Json::array();
    pythonVersion_.clear();
    numpyVersion_.clear();
    publish();
    std::vector<OperatorDesc> operators;
    try {
      if (executable_.empty()) {
        commit(registry, {});
        return publish();
      }
      const Json result = launch(Clock::now() + std::chrono::milliseconds(timeout_));
      problems_ = result.value("problems", Json::array());
      if (!problems_.is_array())
        throw Failure("python_protocol_error", "Python problems 必须是数组");
      pythonVersion_ = result.at("pythonVersion").get<std::string>();
      numpyVersion_ = result.at("numpyVersion").get<std::string>();
      std::set<std::string> ids;
      Registry validation;
      {
        std::shared_lock<std::shared_mutex> lock(registry.mutex());
        for (const auto& t : registry.types())
          validation.addType(t);
        for (const auto& b : registry.bundles())
          validation.addBundle(b);
        std::set<std::string> oldPython;
        for (const auto& op : registry.pythonOperators())
          oldPython.insert(op.id);
        for (const auto& op : registry.operators()) {
          if (!oldPython.count(op.id)) {
            ids.insert(op.id);
            ids.insert(op.aliases.begin(), op.aliases.end());
          }
        }
      }
      for (const auto& j : result.at("operators")) {
        try {
          auto op = descriptor(j);
          if (!ids.insert(op.id).second)
            throw Failure("python_manifest_invalid", "重复算子 ID: " + op.id);
          validation.setPythonOperators({op});
          const auto errors = validation.validate();
          if (!errors.empty())
            throw Failure("python_manifest_invalid", errors.front());
          operators.push_back(std::move(op));
        } catch (const std::exception& e) {
          problem(e.what(), j.is_object() ? j.value("id", std::string()) : std::string());
        }
      }
      ready_ = true;
    } catch (const Failure& e) {
      worker_.stop();
      problem(e.what(), {}, e.traceback);
    } catch (const std::exception& e) {
      worker_.stop();
      problem(e.what());
    }
    count_ = operators.size();
    commit(registry, std::move(operators));
    return publish();
  }

  OperatorDesc descriptor(const Json& j) {
    OperatorDesc op;
    op.id = j.at("id").get<std::string>();
    if (op.id.rfind("py.", 0) != 0 || op.id.find('.', 3) == std::string::npos)
      throw Failure("python_manifest_invalid", "Python 算子 ID 必须是 py.<pack>.<node>");
    op.version = j.value("version", std::string("1.0.0"));
    op.pack = j.value("pack", std::string("python"));
    op.label = j.value("label", op.id);
    op.category = j.value("category", std::string("Python"));
    op.doc = j.value("doc", std::string());
    op.keywords = j.value("keywords", std::vector<std::string>{});
    auto ports = [&](const Json& list, std::vector<Port>& target) {
      if (!list.is_array())
        throw Failure("python_manifest_invalid", "端口声明必须是数组");
      for (const auto& p : list) {
        Port port;
        port.name = p.at("name").get<std::string>();
        port.type = p.at("type").get<std::string>();
        if (port.type != "PointCloud" && port.type != "Image" && port.type != "Measurement" &&
            port.type != "Record")
          throw Failure("python_manifest_invalid", "Python 不支持端口类型 " + port.type);
        port.label = p.value("label", port.name);
        port.doc = p.value("doc", std::string());
        port.required = p.value("required", true);
        port.contract = p.value("contract", Json::object());
        port.example = p.value("example", Json());
        target.push_back(std::move(port));
      }
    };
    ports(j.at("inputs"), op.inputs);
    ports(j.at("outputs"), op.outputs);
    for (const auto& p : j.value("params", Json::array())) {
      ParamType type;
      if (!parseParamType(p.at("type").get<std::string>(), type))
        throw Failure("python_manifest_invalid", "未知 Python 参数类型");
      auto param = exec::paramFromDecl(p.at("name").get<std::string>(), p);
      param.semantic = p.value("semantic", std::string());
      if (p.contains("roiBackdrop")) {
        const auto& b = p.at("roiBackdrop");
        param.roiBackdrop.dirParam = b.value("dir", std::string());
        param.roiBackdrop.fileParams = b.value("files", std::vector<std::string>{});
        param.roiBackdrop.inputPorts = b.value("inputs", std::vector<std::string>{});
        param.roiBackdrop.label = b.value("label", std::string());
        param.roiBackdrop.labelParam = b.value("labelParam", std::string());
      }
      param.absolute = p.value("absolute", false);
      param.pattern = p.value("pattern", std::string());
      param.visibleWhen = condition(p.value("visibleWhen", Json()));
      param.enabledWhen = condition(p.value("enabledWhen", Json()));
      for (const auto& f : p.value("filters", Json::array()))
        param.filters.push_back(
            {f.at("name").get<std::string>(), f.at("extensions").get<std::vector<std::string>>()});
      param.tuningRole = p.value("tuningRole", std::string());
      param.unitSource = p.value("unitSource", std::string());
      if (p.contains("rows"))
        param.rows = p.at("rows").get<int>();
      if (p.contains("alpha"))
        param.alpha = p.at("alpha").get<bool>();
      op.params.push_back(std::move(param));
    }
    op.capabilities = {true, false, false};
    const auto id = op.id;
    const auto inputs = op.inputs;
    const auto outputs = op.outputs;
    const auto parameters = op.params;
    op.compute = [this, id, inputs, outputs, parameters, definition = j](
                     const Inputs& in, const ParamView& params, Outputs& out, ExecContext& ctx) {
      return execute(id, definition, inputs, outputs, parameters, in, params, out, ctx);
    };
    return op;
  }

  Status execute(const std::string& id, const Json& definition, const std::vector<Port>& inputPorts,
                 const std::vector<Port>& outputPorts, const std::vector<Param>& parameters,
                 const Inputs& inputs, const ParamView& params, Outputs& outputs,
                 ExecContext& ctx) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_);
    std::unique_lock<std::timed_mutex> taskLock(tasks, std::defer_lock);
    while (!taskLock.try_lock_for(std::chrono::milliseconds(20))) {
      if (ctx.cancelled())
        return Status::Error(Phase::Execute, "python_cancelled", "Python 排队任务已取消");
      if (Clock::now() >= deadline)
        return Status::Error(Phase::Execute, "python_timeout", "等待 Python worker 超时");
    }
    try {
      if (ctx.cancelled())
        throw Failure("python_cancelled", "Python 运行已取消");
      if (!worker_.alive()) {
        const Json discovered = launch(deadline, &ctx);
        std::lock_guard<std::mutex> lock(mutex);
        ready_ = true;
        pythonVersion_ = discovered.at("pythonVersion").get<std::string>();
        numpyVersion_ = discovered.at("numpyVersion").get<std::string>();
        publish();
      }
      if (!workerDefinitions_.contains(id) || workerDefinitions_.at(id) != definition)
        throw Failure("python_definition_changed",
                      "Python 节点定义已变化，请手动刷新 Python 节点目录: " + id);
      Directory directory;
      Json encoded = Json::object();
      for (const auto& p : inputPorts) {
        if (inputs.has(p.name))
          encoded[p.name] = encode(inputs.get(p.name), directory.path);
        else if (p.required)
          throw Failure("python_input_invalid", "缺少输入端口 " + p.name);
      }
      Json values = Json::object();
      for (const auto& p : parameters) {
        const auto& value = params.raw().at(p.name);
        values[p.name] =
            p.type == ParamType::Curve ? Json::parse(value.stringValue()) : valueJson(value);
      }
      Json result = worker_.exchange({{"command", "execute"},
                                      {"operatorId", id},
                                      {"inputs", std::move(encoded)},
                                      {"params", std::move(values)},
                                      {"baseDir", ctx.baseDir().u8string()},
                                      {"threadBudget", ctx.threadBudget()},
                                      {"requestDir", directory.path.u8string()}},
                                     deadline, &ctx);
      const auto& encodedOutputs = result.at("outputs");
      if (!encodedOutputs.is_object() || encodedOutputs.size() != outputPorts.size())
        throw Failure("python_output_invalid", "输出端口字典与声明不符");
      std::vector<std::pair<std::string, Data>> decoded;
      for (const auto& p : outputPorts)
        decoded.emplace_back(p.name, decode(encodedOutputs.at(p.name), p.type, directory.path));
      for (auto& output : decoded)
        outputs.set(output.first, std::move(output.second));
      return Status::Ok();
    } catch (const Failure& e) {
      worker_.drainLogs(&ctx);
      if (e.code == "python_timeout" || e.code == "python_worker_crashed" ||
          e.code == "python_cancelled" || e.code == "python_protocol_error") {
        worker_.stop(&ctx);
        std::lock_guard<std::mutex> lock(mutex);
        ready_ = false;
        publish();
      }
      return Status::Error(Phase::Execute, e.code, e.what());
    } catch (const std::exception& e) {
      worker_.stop(&ctx);
      std::lock_guard<std::mutex> lock(mutex);
      ready_ = false;
      publish();
      return Status::Error(Phase::Execute, "python_output_invalid", e.what());
    }
  }
};

Runtime& runtime() {
  static Runtime instance;
  return instance;
}
} // namespace

Json configure(Registry& registry, const std::string& config) {
  return runtime().configure(registry, config);
}
Json status() { return runtime().snapshot(); }
Json refresh(Registry& registry) { return runtime().refresh(registry); }
} // namespace lyflow::python_runtime
