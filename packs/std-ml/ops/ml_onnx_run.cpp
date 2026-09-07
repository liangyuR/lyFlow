// ml.onnx_run：一次 ONNX 前向。会话按「路径 + mtime + 线程数」缓存，
// 每次运行重新加载十几 MB 的模型撑不住 live preview（ADR-0015）。
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ops.h"

namespace lyflow::ops {
namespace {

namespace fs = std::filesystem;

std::string fileStamp(const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec) return {};
  const auto mtime = fs::last_write_time(p, ec);
  if (ec) return {};
  return std::to_string(size) + ":" +
         std::to_string(mtime.time_since_epoch().count());
}

/// 一个缓存住的会话。名字取自模型自己的第 0 个输入/输出，参数留空时用它们。
struct Cached {
  Ort::Session session;
  Ort::AllocatorWithDefaultOptions allocator;
  std::string inputName;
  std::string outputName;
  std::mutex mutex;  // Ort::Session::Run 不是并发安全的

  Cached(Ort::Env& env, const fs::path& model, int threads)
      : session(makeSession(env, model, threads)) {
    inputName = session.GetInputNameAllocated(0, allocator).get();
    outputName = session.GetOutputNameAllocated(0, allocator).get();
  }

  static Ort::Session makeSession(Ort::Env& env, const fs::path& model, int threads) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetInterOpNumThreads(threads);
    return Ort::Session(env, model.c_str(), options);
  }
};

Ort::Env& sharedEnv() {
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "lyflow_std_ml");
  return env;
}

std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}

/// 构造失败返回 nullptr、message 是一句人话；指针归缓存所有。
Cached* sessionFor(const fs::path& model, int threads, std::string* message) {
  const std::string stamp = fileStamp(model);
  if (stamp.empty()) {
    *message = "模型文件读不到: " + model.string();
    return nullptr;
  }
  const std::string key = model.string() + "|" + stamp + "|" + std::to_string(threads);

  static std::unordered_map<std::string, std::unique_ptr<Cached>> cache;
  std::lock_guard<std::mutex> lock(cacheMutex());
  auto it = cache.find(key);
  if (it != cache.end()) return it->second.get();
  try {
    auto made = std::make_unique<Cached>(sharedEnv(), model, threads);
    return cache.emplace(key, std::move(made)).first->second.get();
  } catch (const std::exception& e) {
    *message = "加载模型失败 " + model.string() + ": " + e.what();
    return nullptr;
  }
}

std::string externalKey(const ParamView& params) { return fileStamp(params.path("modelPath")); }

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
               ExecContext& ctx) {
  const fs::path model = params.path("modelPath");
  if (model.empty()) {
    return Status::Error(Phase::Execute, "bad_param", "没有给模型文件", "modelPath");
  }
  std::error_code ec;
  if (!fs::is_regular_file(model, ec)) {
    return Status::Error(Phase::Execute, "bad_param", "模型文件不存在: " + model.string(),
                         "modelPath");
  }

  const Tensor* in = inputs.get("input").asTensor();
  if (in == nullptr || !in->consistent()) {
    return Status::Error(Phase::Execute, "bad_input", "输入张量的形状与数据长度对不上", {},
                         "input");
  }
  if (in->shape.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "输入张量没有形状", {}, "input");
  }

  const int threads =
      static_cast<int>(std::max<std::int64_t>(1, params.integer("intraOpThreads")));
  std::string message;
  Cached* cached = sessionFor(model, threads, &message);
  if (cached == nullptr) {
    return Status::Error(Phase::Execute, "bad_param", message, "modelPath");
  }

  const std::string& inName =
      params.text("inputName").empty() ? cached->inputName : params.text("inputName");
  const std::string& outName =
      params.text("outputName").empty() ? cached->outputName : params.text("outputName");

  Tensor out;
  try {
    const Ort::MemoryInfo memory =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<float> data = in->data;  // CreateTensor 要非 const 指针，且不拷贝
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, data.data(), data.size(),
                                                       in->shape.data(), in->shape.size());
    const char* inNames[] = {inName.c_str()};
    const char* outNames[] = {outName.c_str()};

    std::vector<Ort::Value> results;
    {
      std::lock_guard<std::mutex> lock(cached->mutex);
      results = cached->session.Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
    }
    if (results.empty() || !results.front().IsTensor()) {
      return Status::Error(Phase::Execute, "bad_input", "模型没有输出张量", {}, "input");
    }
    out.shape = results.front().GetTensorTypeAndShapeInfo().GetShape();
    const std::size_t n = out.elementCount();
    const float* raw = results.front().GetTensorData<float>();
    out.data.assign(raw, raw + n);
  } catch (const Ort::Exception& e) {
    // 形状不符、名字不存在都从这里出来，portName 指回输入端口
    return Status::Error(Phase::Execute, "bad_input", std::string("推理失败: ") + e.what(), {},
                         "input");
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "internal", std::string("推理失败: ") + e.what());
  }

  ctx.log(LogLevel::Info, in->shapeString() + " -> " + out.shapeString());
  outputs.set("output", Data::tensor(std::move(out)));
  return Status::Ok();
}

Param textParam(const char* name, const char* label, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::String;
  p.label = label;
  p.doc = doc;
  p.def = Value::text("");
  return p;
}

}  // namespace

void registerMlOnnxRun(Registry& r) {
  OperatorDesc op;
  op.id = "ml.onnx_run";
  op.version = "1.0.0";
  op.label = "ONNX 推理";
  op.category = "机器学习/推理";
  // 关键词里不能出现小写的 i-n-f：manifest 的序列化用例是子串匹配（test_executor.cpp:252）
  op.keywords = {"onnx", "Inference", "model", "tensor", "推理", "模型"};
  op.doc =
      "跑一次 ONNX 前向：float32 张量进、float32 张量出。"
      "会话按「模型路径 + mtime + 线程数」缓存，改模型文件会自动重载。\n"
      "输入输出名留空就用模型自己的第 0 个。形状不符时报 bad_input，"
      "错误消息是 onnxruntime 的原话。";

  op.inputs = {Port{"input", "Tensor", "Input", "喂给模型的张量。", true}};
  op.outputs = {Port{"output", "Tensor", "Output", "模型的输出张量。", true}};

  Param model;
  model.name = "modelPath";
  model.type = ParamType::Path;
  model.label = "Model";
  model.doc = "ONNX 模型文件。";
  model.def = Value::text("");
  model.mode = "open";
  model.filters = {FileFilter{"ONNX", {"onnx"}}};

  Param threads;
  threads.name = "intraOpThreads";
  threads.type = ParamType::Int;
  threads.label = "Intra-op Threads";
  threads.doc =
      "算子内并行的线程数，同时用作 inter-op。默认 1 —— 归约顺序固定，同输入逐位可复现。";
  threads.def = Value::integer(1);
  threads.min = 1.0;

  op.params = {
      model,
      textParam("inputName", "Input Name", "模型的输入名。留空用第 0 个。"),
      textParam("outputName", "Output Name", "模型的输出名。留空用第 0 个。"),
      threads,
  };
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
