// std-ml 包的算子测试。真跑一次推理要模型文件，所以那一条按环境变量跳过。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_ml {
void registerPackOps(Registry& r);
}

namespace {

using namespace lyflow;

class NullContext final : public ExecContext {
 public:
  bool cancelled() const override { return false; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return baseDir_; }
  int threadBudget() const override { return 1; }

 private:
  std::filesystem::path baseDir_;
};

const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::std_ml::registerPackOps(reg);
    return reg;
  }();
  return r;
}

struct Call {
  std::unordered_map<std::string, Data> inputs;
  std::unordered_map<std::string, Data> outputs;
  ParamMap params;

  Status run(const std::string& opId, const std::unordered_map<std::string, Value>& overrides = {}) {
    const OperatorDesc* op = packRegistry().find(opId);
    REQUIRE(op != nullptr);
    for (const Param& p : op->params) params[p.name] = p.def;
    for (const auto& [k, v] : overrides) params[k] = v;
    NullContext ctx;
    const std::filesystem::path base;
    ParamView view(params, base);
    Inputs in(inputs);
    Outputs out(outputs);
    return op->compute(in, view, out, ctx);
  }
};

/// 测试模型的位置。没设环境变量时用本机 gap 数据集里的那一个。
std::filesystem::path testModel() {
  if (const char* env = std::getenv("LYFLOW_TEST_ONNX_MODEL")) return std::filesystem::path(env);
  return std::filesystem::path(
      "C:/Users/11601/OneDrive/Documents/DTS/models/v12s0.onnx");
}

}  // namespace

TEST_CASE("ml.onnx_run 注册了，且带 pack 标记") {
  // 裸注册表里没有类型表，所以校验的是全局那一份
  const auto problems = ensureRegistry().validate();
  for (const auto& p : problems) MESSAGE(p);
  CHECK(problems.empty());
  const OperatorDesc* op = ensureRegistry().find("ml.onnx_run");
  REQUIRE(op != nullptr);
  CHECK(op->pack == "std-ml@0.1.0");
  CHECK(op->inputs.at(0).type == "Tensor");
  CHECK(op->outputs.at(0).type == "Tensor");
  CHECK(op->externalKey != nullptr);
  CHECK(ensureRegistry().findType("Tensor") != nullptr);
}

TEST_CASE("ml.onnx_run 没给模型时报 bad_param 而不是崩") {
  Call c;
  Tensor t;
  t.shape = {1, 2};
  t.data = {0.0f, 0.0f};
  c.inputs["input"] = Data::tensor(t);
  const Status s = c.run("ml.onnx_run");
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_param");
  CHECK(s.paramPath == "modelPath");
}

TEST_CASE("ml.onnx_run 对 [2,6,1280] 零张量给出 [2,8,1280]") {
  const std::filesystem::path model = testModel();
  if (!std::filesystem::is_regular_file(model)) {
    MESSAGE("跳过：没有测试模型 " << model.string() << "（设 LYFLOW_TEST_ONNX_MODEL）");
    return;
  }
  Call c;
  Tensor t;
  t.shape = {2, 6, 1280};
  t.data.assign(2 * 6 * 1280, 0.0f);
  c.inputs["input"] = Data::tensor(t);
  const Status s = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CAPTURE(s.message);
  REQUIRE(s.ok);
  const Tensor* out = c.outputs["output"].asTensor();
  REQUIRE(out != nullptr);
  CHECK(out->shape == std::vector<std::int64_t>{2, 8, 1280});
  CHECK(out->consistent());
}

TEST_CASE("ml.onnx_run 形状不符时报 bad_input 并指回输入端口") {
  const std::filesystem::path model = testModel();
  if (!std::filesystem::is_regular_file(model)) return;
  Call c;
  Tensor t;
  t.shape = {2, 3, 7};
  t.data.assign(2 * 3 * 7, 0.0f);
  c.inputs["input"] = Data::tensor(t);
  const Status s = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "input");
}
