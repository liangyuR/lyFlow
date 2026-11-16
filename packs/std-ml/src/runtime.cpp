#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lyflow_ml/runtime.h"
#include "runtime_stats.h"
#include "session_cache.h"

namespace lyflow::ml {
namespace {
namespace fs = std::filesystem;
std::atomic<std::size_t> createdSessions{0};
std::string fingerprint(const fs::path& p) {
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
    createdSessions.fetch_add(1, std::memory_order_relaxed);
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
  const std::string stamp = fingerprint(model);
  if (stamp.empty()) {
    *message = "模型文件读不到: " + model.string();
    return nullptr;
  }
  const ops::detail::SessionKey key{ops::detail::sessionModelPath(model).generic_u8string(), stamp,
                                    threads};
  try {
    Ort::Env& env = sharedEnv();
    static ops::detail::SessionCache<Cached> cache(8);
    return cache.getOrCreate(key, [&] { return std::make_shared<Cached>(env, model, threads); });
  } catch (const std::exception& e) {
    *message = "加载模型失败 " + model.string() + ": " + e.what();
    return nullptr;
  }
}

} // namespace

std::string modelFingerprint(const std::filesystem::path& model) { return fingerprint(model); }
namespace detail {
std::size_t sessionCreationCount() { return createdSessions.load(std::memory_order_relaxed); }
}

Status inferTensor(const Tensor* in, const RuntimeOptions& options, Tensor& output,
                   ExecContext& ctx) {
  const fs::path model = options.modelPath;
  if (model.empty()) {
    return Status::Error(Phase::Execute, "bad_param", "没有给模型文件", "modelPath");
  }
  std::error_code ec;
  if (!fs::is_regular_file(model, ec)) {
    return Status::Error(Phase::Execute, "bad_param", "模型文件不存在: " + model.string(),
                         "modelPath");
  }

  if (in == nullptr || !in->consistent()) {
    return Status::Error(Phase::Execute, "bad_input", "输入张量的形状与数据长度对不上", {},
                         "input");
  }
  if (in->shape.empty()) {
    return Status::Error(Phase::Execute, "bad_input", "输入张量没有形状", {}, "input");
  }

  const auto batchMode = options.batchMode;
  if (batchMode != "model" && batchMode != "serial")
    return Status::Error(Phase::Execute, "bad_param", "批处理模式必须为model或serial", "batchMode");
  const bool serial = batchMode == "serial";
  std::vector<std::int64_t> runShape = in->shape;
  constexpr std::size_t maxBatchElements = 32U * 1024U * 1024U;
  if (serial) {
    if (runShape.size() < 2 || runShape.size() > 8 || runShape[0] < 0 || runShape[0] > 256 ||
        std::any_of(runShape.begin() + 1, runShape.end(), [](auto d) { return d <= 0; }) ||
        in->data.size() > maxBatchElements ||
        !std::all_of(in->data.begin(), in->data.end(),
                     [](float value) { return std::isfinite(value); }))
      return Status::Error(Phase::Execute, "bad_input",
                           "串行批处理需要2..8维Tensor、0..256项与最多32Mi个float32元素", {},
                           "input");
    runShape[0] = 1;
  }
  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "ONNX推理已取消");

  const int requested = static_cast<int>(std::min<std::int64_t>(
      std::numeric_limits<int>::max(), std::max<std::int64_t>(1, options.intraOpThreads)));
  const int threads =
      serial ? std::min({requested, 16, std::max(1, ctx.threadBudget())}) : requested;
  std::string message;
  const auto cached = sessionFor(model, threads, &message);
  if (cached == nullptr) {
    return Status::Error(Phase::Execute, "bad_param", message, "modelPath");
  }

  if (options.modelContract == "yolox-nano-0.1.1rc0") {
    try {
      if (cached->session.GetInputCount() != 1 || cached->session.GetOutputCount() != 1)
        return Status::Error(Phase::Execute, "bad_param",
                             "YOLOX-Nano 契约要求恰好一个输入和一个输出", "modelPath");
      const auto inputType = cached->session.GetInputTypeInfo(0);
      const auto outputType = cached->session.GetOutputTypeInfo(0);
      const auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
      const auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
      if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          inputInfo.GetShape() != std::vector<std::int64_t>({1, 3, 416, 416}) ||
          outputInfo.GetShape() != std::vector<std::int64_t>({1, 3549, 85}) ||
          runShape != inputInfo.GetShape())
        return Status::Error(Phase::Execute, "bad_param",
                             "YOLOX-Nano 契约需要 float32 [1,3,416,416] → [1,3549,85]",
                             "modelPath");
    } catch (const Ort::Exception& e) {
      return Status::Error(Phase::Execute, "bad_param", std::string("模型契约不匹配: ") + e.what(),
                           "modelPath");
    }
  }

  if (options.modelContract == "yolo-detection") {
    try {
      if (cached->session.GetInputCount() != 1 || cached->session.GetOutputCount() != 1)
        return Status::Error(Phase::Execute, "bad_param", "YOLO检测契约要求恰好一个输入和一个输出",
                             "modelPath");
      const auto inputType = cached->session.GetInputTypeInfo(0);
      const auto outputType = cached->session.GetOutputTypeInfo(0);
      const auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
      const auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
      const auto inputShape = inputInfo.GetShape(), outputShape = outputInfo.GetShape();
      if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          inputShape.size() != 4 || inputShape[0] != 1 || inputShape[1] != 3 ||
          inputShape[2] < 32 || inputShape[2] > 2048 || inputShape[3] < 32 ||
          inputShape[3] > 2048 || inputShape[2] % 32 || inputShape[3] % 32 ||
          runShape != inputShape || outputShape.size() != 3 || outputShape[0] != 1)
        return Status::Error(Phase::Execute, "bad_param",
                             "YOLO检测需要静态float32 RGB NCHW batch=1，宽高为32倍数且32..2048",
                             "modelPath");
      std::int64_t anchors = 0;
      for (int stride : {8, 16, 32})
        anchors += (inputShape[2] / stride) * (inputShape[3] / stride);
      const bool raw = outputShape[1] >= 5 && outputShape[1] <= 516 && outputShape[2] == anchors;
      const bool end2end = outputShape[1] >= 1 && outputShape[1] <= 4096 && outputShape[2] == 6;
      if ((!raw && !end2end) || outputInfo.GetElementCount() > 16u * 1024u * 1024u)
        return Status::Error(Phase::Execute, "bad_param",
                             "YOLO检测输出需要[1,4+C,N]常规头或[1,N,6]端到端格式", "modelPath");
    } catch (const Ort::Exception& e) {
      return Status::Error(Phase::Execute, "bad_param",
                           std::string("YOLO模型契约不匹配: ") + e.what(), "modelPath");
    }
  }

  const auto contractName = options.modelContract;
  const bool ocr =
      contractName == "ocr-db" || contractName == "ocr-direction-2" || contractName == "ocr-ctc";
  if (serial || contractName == "pphumanseg_2023mar" || ocr) {
    try {
      if (cached->session.GetInputCount() != 1 || cached->session.GetOutputCount() != 1)
        return Status::Error(Phase::Execute, "bad_param",
                             "串行批处理与分割契约要求恰好一个输入和一个输出", "modelPath");
      const auto inputType = cached->session.GetInputTypeInfo(0);
      const auto outputType = cached->session.GetOutputTypeInfo(0);
      const auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
      const auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
      const auto shape = inputInfo.GetShape();
      const auto outputShape = outputInfo.GetShape();
      if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          shape.size() != runShape.size() || shape.empty() || (shape[0] != 1 && shape[0] != -1) ||
          outputShape.size() < 2 || (outputShape[0] != 1 && outputShape[0] != -1))
        return Status::Error(Phase::Execute, "bad_param", "模型需要单float32输入/输出及显式batch轴",
                             "modelPath");
      for (std::size_t axis = 0; axis < shape.size(); ++axis)
        if (shape[axis] != -1 && shape[axis] != runShape[axis])
          return Status::Error(Phase::Execute, "bad_param", "输入与模型固定维度不匹配",
                               "modelPath");
      if (options.modelContract == "pphumanseg_2023mar" &&
          (shape != std::vector<std::int64_t>({1, 3, 192, 192}) ||
           outputShape != std::vector<std::int64_t>({1, 2, 192, 192})))
        return Status::Error(Phase::Execute, "bad_param",
                             "PPHumanSeg2023mar要求[1,3,192,192]→[1,2,192,192]float32概率",
                             "modelPath");
      if (ocr) {
        if (shape.size() != 4 || shape[1] != 3 || runShape[0] != 1 || runShape[1] != 3)
          return Status::Error(Phase::Execute, "bad_param", "OCR模型需要RGB NCHW batch1单头float32",
                               "modelPath");
        if (contractName == "ocr-db" &&
            (runShape[2] < 32 || runShape[3] < 32 || runShape[2] > 4096 || runShape[3] > 4096 ||
             runShape[2] % 32 || runShape[3] % 32 || outputShape.size() != 4 ||
             outputShape[1] != 1))
          return Status::Error(Phase::Execute, "bad_param",
                               "DB模型要求32倍数的32..4096输入与单通道概率输出", "modelPath");
        if (contractName == "ocr-direction-2" && (runShape[2] != 48 || runShape[3] != 192 ||
                                                  outputShape.size() != 2 || outputShape[1] != 2))
          return Status::Error(Phase::Execute, "bad_param", "方向模型要求RGB48×192与两个方向概率",
                               "modelPath");
        if (contractName == "ocr-ctc" &&
            (runShape[2] != 48 || runShape[3] < 8 || runShape[3] > 4096 ||
             outputShape.size() != 3 || outputShape[2] < 2 || outputShape[2] > 65536))
          return Status::Error(Phase::Execute, "bad_param",
                               "CTC模型要求RGB高度48、宽8..4096及NTC字典输出", "modelPath");
      }
    } catch (const Ort::Exception& e) {
      return Status::Error(Phase::Execute, "bad_param", std::string("模型契约不匹配: ") + e.what(),
                           "modelPath");
    }
  }

  const std::string& inName = options.inputName.empty() ? cached->inputName : options.inputName;
  const std::string& outName = options.outputName.empty() ? cached->outputName : options.outputName;
  if (serial && (inName != cached->inputName || outName != cached->outputName))
    return Status::Error(Phase::Execute, "bad_param", "串行批处理的输入输出名必须匹配唯一模型端口",
                         "inputName");

  Tensor out;
  try {
    const bool strictShape = serial || contractName != "generic";
    std::vector<std::int64_t> declaredOutput;
    if (strictShape) {
      const auto declaredType = cached->session.GetOutputTypeInfo(0);
      declaredOutput = declaredType.GetTensorTypeAndShapeInfo().GetShape();
    }
    std::lock_guard<std::mutex> lock(cached->mutex);
    const std::size_t batches = serial ? static_cast<std::size_t>(in->shape[0]) : 1;
    const std::size_t stride = batches ? in->data.size() / batches : 0;
    if (serial && batches == 0)
      out.shape = {0};
    std::vector<std::int64_t> itemShape;
    for (std::size_t item = 0; item < batches; ++item) {
      if (ctx.cancelled())
        return Status::Error(Phase::Execute, "cancelled", "ONNX推理已在批次边界取消");
      auto& data = cached->inputScratch;
      data.assign(in->data.begin() + item * stride, in->data.begin() + (item + 1) * stride);
      Ort::Value input = Ort::Value::CreateTensor<float>(
          cached->inputMemory, data.data(), data.size(), runShape.data(), runShape.size());
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
      const auto currentShape = info.GetShape();
      if (strictShape) {
        if (declaredOutput.size() != currentShape.size())
          return Status::Error(Phase::Execute, "bad_input", "模型实际输出rank与声明不符",
                               "outputName");
        for (std::size_t axis = 0; axis < currentShape.size(); ++axis)
          if (declaredOutput[axis] >= 0 && declaredOutput[axis] != currentShape[axis])
            return Status::Error(Phase::Execute, "bad_input", "模型实际输出维度与声明不符",
                                 "outputName");
        if (contractName == "pphumanseg_2023mar" &&
            currentShape != std::vector<std::int64_t>({1, 2, 192, 192}))
          return Status::Error(Phase::Execute, "bad_input", "PPHumanSeg实际输出不符合固定概率头",
                               "outputName");
        if (contractName == "ocr-direction-2" && currentShape != std::vector<std::int64_t>({1, 2}))
          return Status::Error(Phase::Execute, "bad_input", "方向模型实际输出必须为[1,2]",
                               "outputName");
        if (contractName == "ocr-db" &&
            currentShape != std::vector<std::int64_t>({1, 1, runShape[2], runShape[3]}))
          return Status::Error(Phase::Execute, "bad_input", "DB模型实际概率图尺寸与输入不一致",
                               "outputName");
        if (contractName == "ocr-ctc" &&
            (currentShape.size() != 3 || currentShape[0] != 1 || currentShape[1] < 1 ||
             currentShape[1] > 8192 || currentShape[2] < 2 || currentShape[2] > 65536))
          return Status::Error(Phase::Execute, "bad_input", "CTC模型实际输出不符合NTC契约",
                               "outputName");
      }
      Tensor shapeCheck;
      shapeCheck.shape = currentShape;
      const std::size_t n = info.GetElementCount();
      if (n != shapeCheck.elementCount() || n > out.data.max_size() ||
          std::any_of(currentShape.begin(), currentShape.end(),
                      [](std::int64_t d) { return d < 0; })) {
        return Status::Error(Phase::Execute, "bad_input", "模型输出形状或元素数量不受支持",
                             "outputName");
      }
      if (serial && (currentShape.size() < 2 || currentShape[0] != 1 ||
                     n > maxBatchElements - out.data.size()))
        return Status::Error(Phase::Execute, "resource_limit",
                             "串行模型输出需要batch=1且累计不超过32Mi个float32元素", "outputName");
      if (item == 0) {
        itemShape = currentShape;
        out.shape = currentShape;
        if (serial)
          out.shape[0] = static_cast<std::int64_t>(batches);
      } else if (currentShape != itemShape) {
        return Status::Error(Phase::Execute, "bad_input", "各批模型输出形状不一致", "outputName");
      }
      if (n > 0) {
        const float* raw = results.front().GetTensorData<float>();
        if (raw == nullptr) {
          return Status::Error(Phase::Execute, "bad_input", "模型输出张量没有数据", "outputName");
        }
        if (serial && !std::all_of(raw, raw + n, [](float value) { return std::isfinite(value); }))
          return Status::Error(Phase::Execute, "bad_input", "串行模型输出含非有限值", "outputName");
        out.data.insert(out.data.end(), raw, raw + n);
      }
      if (serial)
        ctx.progress(static_cast<float>(item + 1) / static_cast<float>(batches));
    }
  } catch (const Ort::Exception& e) {
    // 形状不符、名字不存在都从这里出来，portName 指回输入端口
    return Status::Error(Phase::Execute, "bad_input", std::string("推理失败: ") + e.what(), {},
                         "input");
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "internal", std::string("推理失败: ") + e.what());
  }

  if (ctx.cancelled())
    return Status::Error(Phase::Execute, "cancelled", "ONNX推理结束时已取消");

  ctx.log(LogLevel::Info, in->shapeString() + " -> " + out.shapeString());
  output = std::move(out);
  return Status::Ok();
}

} // namespace lyflow::ml
