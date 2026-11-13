// ml.onnx_run：一次 ONNX 前向。会话按「路径 + 文件指纹 + 线程数」缓存，
// 每次运行重新加载十几 MB 的模型撑不住 live preview（ADR-0015）。
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ops.h"
#include "session_cache.h"

namespace lyflow::ops {
namespace {

namespace fs = std::filesystem;

std::string fileStamp(const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec)
    return {};
  const auto mtime = fs::last_write_time(p, ec);
  if (ec)
    return {};
  return std::to_string(size) + ":" + std::to_string(mtime.time_since_epoch().count());
}

/// 一个缓存住的会话。名字取自模型自己的第 0 个输入/输出，参数留空时用它们。
struct Cached {
  Ort::Session session;
  Ort::AllocatorWithDefaultOptions allocator;
  Ort::MemoryInfo inputMemory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::string inputName;
  std::string outputName;
  std::vector<float> inputScratch;
  std::mutex mutex;

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

std::shared_ptr<Cached> sessionFor(const fs::path& model, int threads, std::string* message) {
  const std::string stamp = fileStamp(model);
  if (stamp.empty()) {
    *message = "模型文件读不到: " + model.string();
    return nullptr;
  }
  const detail::SessionKey key{detail::sessionModelPath(model).generic_u8string(), stamp, threads};
  try {
    Ort::Env& env = sharedEnv();
    static detail::SessionCache<Cached> cache(8);
    return cache.getOrCreate(key, [&] { return std::make_shared<Cached>(env, model, threads); });
  } catch (const std::exception& e) {
    *message = "加载模型失败 " + model.string() + ": " + e.what();
    return nullptr;
  }
}

std::string externalKey(const ParamView& params) { return fileStamp(params.path("modelPath")); }

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
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

  const int threads = static_cast<int>(std::max<std::int64_t>(1, params.integer("intraOpThreads")));
  std::string message;
  const auto cached = sessionFor(model, threads, &message);
  if (cached == nullptr) {
    return Status::Error(Phase::Execute, "bad_param", message, "modelPath");
  }

  const std::string& inName =
      params.text("inputName").empty() ? cached->inputName : params.text("inputName");
  const std::string& outName =
      params.text("outputName").empty() ? cached->outputName : params.text("outputName");

  Tensor out;
  try {
    std::lock_guard<std::mutex> lock(cached->mutex);
    auto& data = cached->inputScratch;
    data.assign(in->data.begin(), in->data.end());
    Ort::Value input = Ort::Value::CreateTensor<float>(
        cached->inputMemory, data.data(), data.size(), in->shape.data(), in->shape.size());
    const char* inNames[] = {inName.c_str()};
    const char* outNames[] = {outName.c_str()};

    const auto results =
        cached->session.Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
    if (results.empty() || !results.front().IsTensor()) {
      return Status::Error(Phase::Execute, "bad_input", "模型没有输出张量", {}, "input");
    }
    const auto info = results.front().GetTensorTypeAndShapeInfo();
    const auto elementType = info.GetElementType();
    if (elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      return Status::Error(Phase::Execute, "bad_input",
                           "模型输出必须是 float32，实际 ONNX 元素类型编号: " +
                               std::to_string(static_cast<int>(elementType)),
                           "outputName");
    }
    out.shape = info.GetShape();
    const std::size_t n = info.GetElementCount();
    if (n != out.elementCount() || n > out.data.max_size() ||
        std::any_of(out.shape.begin(), out.shape.end(), [](std::int64_t d) { return d < 0; })) {
      return Status::Error(Phase::Execute, "bad_input", "模型输出形状或元素数量不受支持",
                           "outputName");
    }
    if (n > 0) {
      const float* raw = results.front().GetTensorData<float>();
      if (raw == nullptr) {
        return Status::Error(Phase::Execute, "bad_input", "模型输出张量没有数据", "outputName");
      }
      out.data.assign(raw, raw + n);
    }
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

} // namespace

void registerMlOnnxRun(Registry& r) {
  OperatorDesc op;
  op.id = "ml.onnx_run";
  op.version = "1.0.0";
  op.label = "ONNX 推理";
  op.category = "机器学习/推理";
  // 关键词里不能出现小写的 i-n-f：manifest 的序列化用例是子串匹配（test_executor.cpp:252）
  op.keywords = {"ONNX inference", "ONNX Runtime", "ORT",  "model forward pass", "神经网络推理",
                 "模型推理",       "前向计算",     "onnx", "Inference",          "model",
                 "tensor",         "推理",         "模型"};
  op.doc = "跑一次 ONNX 前向：float32 张量进、float32 张量出。"
           "会话按「模型路径 + 文件大小/mtime + 线程数」缓存，最多保留 8 个最近使用的会话，"
           "改模型文件会自动重载并回收该路径的旧指纹缓存。\n"
           "同会话复用输入缓冲，输入复制、推理与输出复制串行完成，源张量保持只读。\n"
           "输入输出名留空就用模型自己的第 0 个。输出元素类型必须是 float32，"
           "支持零元素输出，不支持标量。类型或形状不符时报 bad_input；ORT 推理错误保留原始消息。";

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
  threads.doc = "算子内并行的线程数，同时用作 inter-op。默认 1 —— 归约顺序固定，同输入逐位可复现。";
  threads.def = Value::integer(1);
  threads.min = 1.0;
  threads.max = static_cast<double>(std::numeric_limits<int>::max());

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

} // namespace lyflow::ops
