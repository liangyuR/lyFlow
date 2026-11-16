#include <limits>
#include <utility>

#include "lyflow_ml/runtime.h"
#include "ops.h"

namespace lyflow::ops {
namespace {
std::string externalKey(const ParamView& params) {
  return ml::modelFingerprint(params.path("modelPath"));
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  ml::RuntimeOptions options;
  options.modelPath = params.path("modelPath");
  options.inputName = params.text("inputName");
  options.outputName = params.text("outputName");
  options.intraOpThreads = params.integer("intraOpThreads");
  options.modelContract = params.choice("modelContract");
  options.batchMode = params.choice("batchMode");
  Tensor output;
  const auto status = ml::inferTensor(inputs.get("input").asTensor(), options, output, ctx);
  if (!status.ok)
    return status;
  outputs.set("output", Data::tensor(std::move(output)));
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
           "支持零元素输出，不支持标量。类型或形状不符时报 bad_input；ORT 推理错误保留原始消息。\n"
           "serial批处理要求单输入/输出、前导batch轴和有限float32，最多256项及32Mi元素；"
           "对固定batch1模型逐项调用同一会话并拼接输出，空批次验证模型后返回shape=[0]且不推理。"
           "取消在推理前后及项边界检查，不能中断已经进入ORT的单次调用。";

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

  Param contract;
  contract.name = "modelContract";
  contract.type = ParamType::Enum;
  contract.label = "模型契约";
  contract.def = Value::text("generic");
  contract.options = {
      {"generic", "通用张量", "沿用现有指定输入输出名约定。"},
      {"yolox-nano-0.1.1rc0", "YOLOX-Nano 0.1.1rc0",
       "固定单输入/输出、位深与形状，不静默丢弃其他输出。"},
      {"yolo-detection", "YOLOv8 / YOLO11 / YOLO26", "静态batch=1 float32常规或端到端检测输出。"}};

  contract.options.push_back(
      {"pphumanseg_2023mar", "PPHumanSeg2023mar", "固定RGB192×192单头双类别概率。"});
  contract.options.push_back({"ocr-db", "DB文字检测", "单float32 NCHW输入及单通道概率输出。"});
  contract.options.push_back(
      {"ocr-direction-2", "文字方向分类", "单float32 RGB48×192输入及双类概率输出。"});
  contract.options.push_back({"ocr-ctc", "CTC文字识别", "单float32 RGB高度48输入及NTC字典输出。"});
  Param batch;
  batch.name = "batchMode";
  batch.type = ParamType::Enum;
  batch.label = "批处理";
  batch.def = Value::text("model");
  batch.options = {{"model", "模型原生批次", "沿用模型维度。"},
                   {"serial", "逐项串行推理",
                    "前导轴最多256项；固定batch1模型复用同会话逐项运行。空批次不推理。"}};

  op.params = {
      model,
      textParam("inputName", "Input Name", "模型的输入名。留空用第 0 个。"),
      textParam("outputName", "Output Name", "模型的输出名。留空用第 0 个。"),
      threads,
      contract,
      batch,
  };
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;

  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
