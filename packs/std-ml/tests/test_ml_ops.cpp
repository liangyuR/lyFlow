// std-ml 包的算子测试。真实推理用例通过 LYFLOW_TEST_ONNX_MODEL 选择模型。
#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "../ops/session_cache.h"
#include "../src/runtime_stats.h"
#include "lyflow_ml/runtime.h"
#include "helpers.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_ml {
void registerPackOps(Registry& r);
}

namespace {

using namespace lyflow;

const Registry& packRegistry() {
  static Registry r = [] {
    Registry reg;
    packs::std_ml::registerPackOps(reg);
    return reg;
  }();
  return r;
}

/// 直接调 compute（test::OpCall），只用本包的注册表。
struct Call : test::OpCall {
  Call() : OpCall(packRegistry()) {}
};

/// 测试模型只认环境变量 LYFLOW_TEST_ONNX_MODEL。没设或文件不在时返回 false，
/// 并打一条 MESSAGE 说明这条用例跳过了 —— 不静默 return，免得「全绿」里藏着没跑的用例。
bool testModel(std::filesystem::path& model) {
  const char* env = std::getenv("LYFLOW_TEST_ONNX_MODEL");
  if (env == nullptr || *env == '\0') {
    MESSAGE("跳过：没设 LYFLOW_TEST_ONNX_MODEL（指向一个 [N,6,1280] → [N,8,1280] 的 onnx 模型）");
    return false;
  }
  model = std::filesystem::u8path(env);
  if (!std::filesystem::is_regular_file(model)) {
    MESSAGE("跳过：LYFLOW_TEST_ONNX_MODEL 指向的文件不存在: " << env);
    return false;
  }
  return true;
}

} // namespace

TEST_CASE("ml.onnx_run 注册了，且带 pack 标记") {
  // 裸注册表里没有类型表，所以校验的是全局那一份
  const auto problems = ensureRegistry().validate();
  for (const auto& p : problems)
    MESSAGE(p);
  CHECK(problems.empty());
  const OperatorDesc* op = ensureRegistry().find("ml.onnx_run");
  REQUIRE(op != nullptr);
  CHECK(op->pack == "std-ml@0.3.1");
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
  std::filesystem::path model;
  if (!testModel(model))
    return;
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
  std::filesystem::path model;
  if (!testModel(model))
    return;
  Call c;
  Tensor t;
  t.shape = {2, 3, 7};
  t.data.assign(2 * 3 * 7, 0.0f);
  c.inputs["input"] = Data::tensor(t);
  const Status s = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.portName == "input");

  Tensor valid;
  valid.shape = {1, 6, 1280};
  valid.data.assign(6 * 1280, 0.0f);
  c.inputs["input"] = Data::tensor(std::move(valid));
  const Status recovered = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CAPTURE(recovered.message);
  REQUIRE(recovered.ok);
  const Tensor* out = c.outputs["output"].asTensor();
  REQUIRE(out != nullptr);
  CHECK(out->shape == std::vector<std::int64_t>{1, 8, 1280});
  CHECK(out->consistent());
}

TEST_CASE("ml.onnx_run 同会话并发交替 batch 和内容不污染输入输出") {
  std::filesystem::path model;
  if (!testModel(model))
    return;
  constexpr std::array<std::int64_t, 4> batches{1, 3, 2, 1};
  std::array<Tensor, batches.size()> originals;
  std::array<Data, batches.size()> inputs;
  std::array<Tensor, batches.size()> expected;
  const std::unordered_map<std::string, Value> params{{"modelPath", Value::text(model.string())},
                                                      {"intraOpThreads", Value::integer(1)}};
  for (std::size_t i = 0; i < batches.size(); ++i) {
    Tensor& input = originals[i];
    input.shape = {batches[i], 6, 1280};
    input.data.resize(static_cast<std::size_t>(batches[i]) * 6 * 1280);
    for (std::size_t j = 0; j < input.data.size(); ++j)
      input.data[j] = static_cast<float>(i + 1) * 0.125f +
                      static_cast<float>(static_cast<int>(j % 23) - 11) * 0.015625f;
    inputs[i] = Data::tensor(input);
    Call c;
    c.inputs["input"] = inputs[i];
    const Status s = c.run("ml.onnx_run", params);
    CAPTURE(s.message);
    REQUIRE(s.ok);
    const Tensor* out = c.outputs["output"].asTensor();
    REQUIRE(out != nullptr);
    REQUIRE(out->consistent());
    expected[i] = *out;
  }

  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::array<std::thread, 4> workers;
  for (std::size_t worker = 0; worker < workers.size(); ++worker) {
    workers[worker] = std::thread([&, worker] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      Call c;
      for (std::size_t iteration = 0; iteration < 6; ++iteration) {
        const std::size_t i = (worker + iteration) % inputs.size();
        c.inputs["input"] = inputs[i];
        const Status s = c.run("ml.onnx_run", params);
        if (!s.ok) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const Tensor* out = c.outputs["output"].asTensor();
        if (out == nullptr || out->shape != expected[i].shape || out->data != expected[i].data)
          failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers)
    worker.join();
  CHECK(failures.load() == 0);
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    REQUIRE(inputs[i].asTensor() != nullptr);
    CHECK(inputs[i].asTensor()->shape == originals[i].shape);
    CHECK(inputs[i].asTensor()->data == originals[i].data);
  }
}

TEST_CASE("ml.onnx_run 拒绝非 float32 输出而不重解释数据") {
  std::filesystem::path reference;
  if (!testModel(reference))
    return;
  const auto model = reference.parent_path() / "cast-to-int64.onnx";
  if (!std::filesystem::is_regular_file(model)) {
    MESSAGE("跳过：测试模型目录缺少 cast-to-int64.onnx，请生成 release 合成夹具");
    return;
  }
  Call c;
  Tensor input;
  input.shape = {1, 6, 1280};
  input.data.assign(6 * 1280, 1.0f);
  c.inputs["input"] = Data::tensor(std::move(input));
  const Status s = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CHECK_FALSE(s.ok);
  CHECK(s.code == "bad_input");
  CHECK(s.paramPath == "outputName");
  CHECK(s.message.find("float32") != std::string::npos);
  CHECK(c.outputs.count("output") == 0);
}

TEST_CASE("ml.onnx_run 保留零元素输出且不读取空数据指针") {
  std::filesystem::path reference;
  if (!testModel(reference))
    return;
  const auto model = reference.parent_path() / "channels-6-to-8.onnx";
  if (!std::filesystem::is_regular_file(model)) {
    MESSAGE("跳过：测试模型目录缺少 channels-6-to-8.onnx，请生成 release 合成夹具");
    return;
  }
  Call c;
  Tensor input;
  input.shape = {0, 6, 1280};
  c.inputs["input"] = Data::tensor(std::move(input));
  const Status s = c.run("ml.onnx_run", {{"modelPath", Value::text(model.string())}});
  CAPTURE(s.message);
  REQUIRE(s.ok);
  const Tensor* out = c.outputs["output"].asTensor();
  REQUIRE(out != nullptr);
  CHECK(out->shape == std::vector<std::int64_t>{0, 8, 1280});
  CHECK(out->data.empty());
  CHECK(out->consistent());
}

TEST_CASE("ONNX serial batching preserves static batch one item order and readonly input") {
  std::filesystem::path reference;
  REQUIRE(testModel(reference));
  const auto model = reference.parent_path() / "channels-6-to-8-static1.onnx";
  REQUIRE(std::filesystem::is_regular_file(model));
  for (int batch : {0, 1, 3}) {
    CAPTURE(batch);
    Tensor input;
    input.shape = {batch, 6, 1280};
    input.data.resize(static_cast<std::size_t>(batch) * 6 * 1280);
    for (std::size_t i = 0; i < input.data.size(); ++i)
      input.data[i] = static_cast<float>(i % 101) * .125f;
    const auto original = input.data;
    Call call;
    call.inputs["input"] = Data::tensor(input);
    const auto status = call.run("ml.onnx_run", {{"modelPath", Value::text(model.u8string())},
                                                 {"batchMode", Value::text("serial")}});
    REQUIRE_MESSAGE(status.ok, status.message);
    const auto* output = call.out("output").asTensor();
    REQUIRE(output != nullptr);
    CHECK(output->consistent());
    CHECK(call.inputs.at("input").asTensor()->data == original);
    if (!batch) {
      CHECK(output->shape == std::vector<std::int64_t>{0});
      CHECK(output->data.empty());
    } else {
      REQUIRE(output->shape == std::vector<std::int64_t>{batch, 8, 1280});
      for (int item = 0; item < batch; ++item) {
        const auto begin = output->data.begin() + item * 8 * 1280;
        CHECK(std::equal(original.begin() + item * 6 * 1280,
                         original.begin() + (item + 1) * 6 * 1280, begin));
        CHECK(std::all_of(begin + 6 * 1280, begin + 8 * 1280,
                          [](float value) { return value == 0; }));
      }
    }
  }
}

TEST_CASE("ONNX serial empty batch still rejects model heads types names and invalid input") {
  std::filesystem::path reference;
  REQUIRE(testModel(reference));
  const auto directory = reference.parent_path();
  Tensor input;
  input.shape = {0, 6, 1280};
  Call call;
  call.inputs["input"] = Data::tensor(input);
  for (const char* name : {"multi-output.onnx", "cast-to-int64.onnx"}) {
    const auto path = directory / name;
    REQUIRE(std::filesystem::is_regular_file(path));
    const auto status = call.run("ml.onnx_run", {{"modelPath", Value::text(path.u8string())},
                                                 {"batchMode", Value::text("serial")}});
    CHECK_FALSE(status.ok);
    CHECK(call.outputs.empty());
  }
  const auto model = directory / "channels-6-to-8-static1.onnx";
  for (const auto& wrong : std::vector<std::pair<std::string, Value>>{
           {"inputName", Value::text("missing")},
           {"outputName", Value::text("missing")},
           {"modelContract", Value::text("pphumanseg_2023mar")},
           {"batchMode", Value::text("invalid")}}) {
    std::unordered_map<std::string, Value> parameters{{"modelPath", Value::text(model.u8string())},
                                                      {"batchMode", Value::text("serial")}};
    parameters[wrong.first] = wrong.second;
    const auto status = call.run("ml.onnx_run", parameters);
    CHECK_FALSE(status.ok);
    CHECK(call.outputs.empty());
  }
  input.shape = {1, 6, 1280};
  input.data.assign(6 * 1280, 0);
  input.data.front() = std::numeric_limits<float>::infinity();
  call.inputs["input"] = Data::tensor(input);
  CHECK_FALSE(call.run("ml.onnx_run", {{"modelPath", Value::text(model.u8string())},
                                       {"batchMode", Value::text("serial")}})
                  .ok);
  CHECK(call.outputs.empty());
}

TEST_CASE("ONNX fixed model contracts validate actual runtime shapes before publishing") {
  std::filesystem::path reference;
  REQUIRE(testModel(reference));
  const auto path = reference.parent_path() / "lying-direction.onnx";
  REQUIRE(std::filesystem::is_regular_file(path));
  Tensor input;
  input.shape = {1, 3, 48, 192};
  input.data.assign(3 * 48 * 192, 0);
  Call call;
  call.inputs["input"] = Data::tensor(input);
  const std::unordered_map<std::string, Value> parameters{
      {"modelPath", Value::text(path.u8string())},
      {"modelContract", Value::text("ocr-direction-2")},
      {"batchMode", Value::text("serial")}};
  const auto rejected = call.run("ml.onnx_run", parameters);
  CHECK_FALSE(rejected.ok);
  CHECK(rejected.code == "bad_input");
  CHECK(rejected.paramPath == "outputName");
  CHECK(call.outputs.empty());
  const auto generic = call.run("ml.onnx_run", {{"modelPath", Value::text(path.u8string())}});
  REQUIRE_MESSAGE(generic.ok, generic.message);
  REQUIRE(call.out("output").asTensor() != nullptr);
  CHECK(call.out("output").asTensor()->shape == std::vector<std::int64_t>{1, 3});
  input.data.front() = -1;
  call.inputs["input"] = Data::tensor(input);
  const auto valid = call.run("ml.onnx_run", parameters);
  REQUIRE_MESSAGE(valid.ok, valid.message);
  CHECK(call.out("output").asTensor()->shape == std::vector<std::int64_t>{1, 2});
}

TEST_CASE("ONNX serial cancellation never publishes a partial batch") {
  std::filesystem::path reference;
  REQUIRE(testModel(reference));
  struct Context : ExecContext {
    bool stop = false;
    int updates = 0;
    std::filesystem::path base;
    bool cancelled() const override { return stop; }
    void progress(float, std::string_view) override {
      ++updates;
      stop = true;
    }
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return base; }
    int threadBudget() const override { return 1; }
  };
  const auto* op = packRegistry().find("ml.onnx_run");
  REQUIRE(op != nullptr);
  ParamMap params;
  for (const auto& param : op->params)
    params[param.name] = param.def;
  params["modelPath"] =
      Value::text((reference.parent_path() / "channels-6-to-8-static1.onnx").u8string());
  params["batchMode"] = Value::text("serial");
  for (int batch : {1, 3}) {
    Context context;
    Tensor input;
    input.shape = {batch, 6, 1280};
    input.data.assign(static_cast<std::size_t>(batch) * 6 * 1280, 1);
    std::unordered_map<std::string, Data> inputs{{"input", Data::tensor(input)}}, outputs;
    Inputs in(inputs);
    Outputs out(outputs);
    ParamView view(params, context.base);
    const auto status = op->compute(in, view, out, context);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "cancelled");
    CHECK(context.updates == 1);
    CHECK(outputs.empty());
    CHECK(inputs.at("input").asTensor()->data == input.data);
  }
}

TEST_CASE("Typed ONNX runtime shares node sessions results diagnostics and cancellation") {
  namespace fs = std::filesystem;
  fs::path fixture;
  REQUIRE(testModel(fixture));
  const auto directory =
      fs::temp_directory_path() /
      ("lyflow-runtime-api-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    fs::path directory;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(directory, ec);
    }
  } cleanup{directory};
  fs::create_directories(directory);
  const auto model = directory / "model.onnx";
  fs::copy_file(fixture, model);
  struct Context : ExecContext {
    bool stop = false, stopAfterProgress = false;
    int updates = 0;
    fs::path base;
    std::vector<std::string> messages;
    bool cancelled() const override { return stop; }
    void progress(float, std::string_view) override {
      ++updates;
      stop = stopAfterProgress;
    }
    void log(LogLevel, std::string message) override { messages.push_back(std::move(message)); }
    const fs::path& baseDir() const override { return base; }
    int threadBudget() const override { return 1; }
  } context;
  Tensor input;
  input.shape = {2, 6, 1280};
  for (int i = 0; i < 2 * 6 * 1280; ++i)
    input.data.push_back(static_cast<float>((i % 23) - 11) * 0.125F);
  const auto original = input;
  ml::RuntimeOptions options;
  options.modelPath = model;
  options.intraOpThreads = 3;
  const auto before = ml::detail::sessionCreationCount();
  Tensor direct;
  REQUIRE(ml::inferTensor(input, options, direct, context).ok);
  REQUIRE(direct.consistent());
  CHECK(ml::detail::sessionCreationCount() == before + 1);
  CHECK(context.messages ==
        std::vector<std::string>{input.shapeString() + " -> " + direct.shapeString()});
  Call node;
  node.inputs = {{"input", Data::tensor(input)}};
  const std::unordered_map<std::string, Value> parameters{
      {"modelPath", Value::text(model.u8string())}, {"intraOpThreads", Value::integer(3)}};
  REQUIRE(node.run("ml.onnx_run", parameters).ok);
  CHECK(ml::detail::sessionCreationCount() == before + 1);
  CHECK(node.out("output").asTensor()->shape == direct.shape);
  CHECK(node.out("output").asTensor()->data == direct.data);
  const auto* descriptor = packRegistry().find("ml.onnx_run");
  REQUIRE(descriptor);
  const auto stamp = ml::modelFingerprint(model);
  CHECK_FALSE(stamp.empty());
  CHECK(descriptor->externalKey(ParamView(node.params, node.base)) == stamp);
  fs::last_write_time(model, fs::last_write_time(model) + std::chrono::seconds(1));
  CHECK(ml::modelFingerprint(model) != stamp);
  REQUIRE(ml::inferTensor(input, options, direct, context).ok);
  REQUIRE(node.run("ml.onnx_run", parameters).ok);
  CHECK(ml::detail::sessionCreationCount() == before + 2);
  const auto checkStatus = [](const Status& typed, const Status& adapted) {
    CHECK_FALSE(typed.ok);
    CHECK(static_cast<int>(typed.phase) == static_cast<int>(adapted.phase));
    CHECK(typed.code == adapted.code);
    CHECK(typed.message == adapted.message);
    CHECK(typed.paramPath == adapted.paramPath);
    CHECK(typed.portName == adapted.portName);
  };
  Tensor sentinel;
  sentinel.shape = {1};
  sentinel.data = {77};
  options.inputName = "missing-port";
  auto invalid = parameters;
  invalid["inputName"] = Value::text(options.inputName);
  checkStatus(ml::inferTensor(input, options, sentinel, context), node.run("ml.onnx_run", invalid));
  CHECK(sentinel.shape == std::vector<std::int64_t>{1});
  CHECK(sentinel.data == std::vector<float>{77});
  CHECK(node.outputs.empty());
  CHECK(input.shape == original.shape);
  CHECK(input.data == original.data);
  options.inputName.clear();
  node.inputs.clear();
  checkStatus(ml::inferTensor(nullptr, options, sentinel, context),
              node.run("ml.onnx_run", parameters));
  node.inputs["input"] = Data::tensor(input);
  for (int batch : {0, 1, 3}) {
    options.modelPath = fixture.parent_path() / "channels-6-to-8-static1.onnx";
    options.batchMode = "serial";
    input.shape[0] = batch;
    input.data.assign(static_cast<std::size_t>(batch) * 6 * 1280, 1);
    node.inputs["input"] = Data::tensor(input);
    REQUIRE(ml::inferTensor(input, options, direct, context).ok);
    REQUIRE(node.run("ml.onnx_run", {{"modelPath", Value::text(options.modelPath.u8string())},
                                     {"batchMode", Value::text("serial")},
                                     {"intraOpThreads", Value::integer(3)}})
                .ok);
    CHECK(node.out("output").asTensor()->shape == direct.shape);
    CHECK(node.out("output").asTensor()->data == direct.data);
    if (batch) {
      Context cancelled;
      cancelled.stopAfterProgress = true;
      const auto status = ml::inferTensor(input, options, sentinel, cancelled);
      CHECK_FALSE(status.ok);
      CHECK(status.code == "cancelled");
      CHECK(cancelled.updates == 1);
      CHECK(sentinel.shape == std::vector<std::int64_t>{1});
      CHECK(sentinel.data == std::vector<float>{77});
    }
  }
}

TEST_CASE("ONNX 会话缓存有界，淘汰保留活跃句柄") {
  using lyflow::ops::detail::SessionCache;
  using lyflow::ops::detail::SessionKey;
  SessionCache<int> cache(2);

  SUBCASE("命中更新 LRU，超过容量只释放最久未用的缓存引用") {
    auto first = cache.getOrCreate({"first", "v1", 1}, [] { return std::make_shared<int>(1); });
    std::weak_ptr<int> second =
        cache.getOrCreate({"second", "v1", 1}, [] { return std::make_shared<int>(2); });
    CHECK(cache.getOrCreate({"first", "v1", 1}, [] { return std::make_shared<int>(0); }) == first);
    cache.getOrCreate({"third", "v1", 1}, [] { return std::make_shared<int>(3); });
    CHECK(second.expired());
    CHECK(cache.size() == 2);

    std::weak_ptr<int> active = first;
    for (int i = 0; i < 20; ++i) {
      cache.getOrCreate({"other-" + std::to_string(i), "v1", 1},
                        [i] { return std::make_shared<int>(i); });
      CHECK(cache.size() == 2);
    }
    CHECK_FALSE(active.expired());
    CHECK(*first == 1);
    first.reset();
    CHECK(active.expired());
  }

  SUBCASE("同路径换指纹回收旧配置，当前指纹的线程配置可共存") {
    auto active = cache.getOrCreate({"model", "v1", 1}, [] { return std::make_shared<int>(1); });
    std::weak_ptr<int> old = active;
    std::weak_ptr<int> oldThreads =
        cache.getOrCreate({"model", "v1", 2}, [] { return std::make_shared<int>(2); });
    cache.getOrCreate({"model", "v2", 1}, [] { return std::make_shared<int>(3); });
    CHECK(cache.size() == 1);
    CHECK(oldThreads.expired());
    CHECK_FALSE(old.expired());
    CHECK(*active == 1);
    active.reset();
    CHECK(old.expired());
    cache.getOrCreate({"model", "v2", 2}, [] { return std::make_shared<int>(4); });
    CHECK(cache.size() == 2);
  }

  SUBCASE("加载失败不污染缓存，后续可重试") {
    auto active = cache.getOrCreate({"model", "v1", 1}, [] { return std::make_shared<int>(1); });
    const SessionKey changed{"model", "v2", 1};
    CHECK_THROWS_AS(
        cache.getOrCreate(
            changed, []() -> std::shared_ptr<int> { throw std::runtime_error("load failed"); }),
        std::runtime_error);
    CHECK(cache.size() == 1);
    CHECK(cache.getOrCreate({"model", "v1", 1}, [] { return std::make_shared<int>(0); }) == active);
    CHECK_FALSE(cache.getOrCreate(changed, [] { return std::shared_ptr<int>{}; }));
    CHECK(cache.size() == 1);
    CHECK(*cache.getOrCreate(changed, [] { return std::make_shared<int>(2); }) == 2);
    CHECK(cache.size() == 1);
  }
}

TEST_CASE("ONNX 会话缓存路径解析与系统实际打开的文件一致（含符号链接后的父目录）") {
  namespace fs = std::filesystem;
  using lyflow::ops::detail::sessionModelPath;
  const auto dir = fs::temp_directory_path() /
                   ("lyflow-session-path-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{dir};
  fs::create_directories(dir / "target" / "child");
  std::ofstream(dir / "model.onnx") << "first";
  std::ofstream(dir / "target" / "model.onnx") << "other";
  CHECK(sessionModelPath(dir / "." / "target" / "child" / ".." / "model.onnx") ==
        sessionModelPath(dir / "target" / "model.onnx"));

  std::error_code ec;
  fs::create_directory_symlink(dir / "target" / "child", dir / "link", ec);
  if (ec) {
    MESSAGE("跳过符号链接断言：无法创建目录符号链接: " << ec.message());
    return;
  }
  // link/.. 指向哪个文件随平台而定：POSIX 先解析链接（得 target/model.onnx），Windows 先按字面消去 ..（得 model.onnx）。
  // 缓存键必须与系统实际打开的文件一致，并与另一个文件区分开。
  const auto throughLink = dir / "link" / ".." / "model.onnx";
  std::string opened;
  std::getline(std::ifstream(throughLink), opened);
  REQUIRE((opened == "first" || opened == "other"));
  const auto actual = opened == "other" ? dir / "target" / "model.onnx" : dir / "model.onnx";
  const auto other = opened == "other" ? dir / "model.onnx" : dir / "target" / "model.onnx";
  CHECK(sessionModelPath(throughLink) == sessionModelPath(actual));
  CHECK(sessionModelPath(throughLink) != sessionModelPath(other));
}

TEST_CASE("ONNX 会话缓存并发创建只保留一个实例，慢创建不阻塞命中") {
  using lyflow::ops::detail::SessionCache;
  SessionCache<int> cache(2);

  SUBCASE("同键并发 miss 只构建一次") {
    std::atomic<int> created{0};
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::array<std::shared_ptr<int>, 8> sessions;
    std::array<std::thread, 8> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) {
      workers[i] = std::thread([&, i] {
        ready.wait();
        sessions[i] = cache.getOrCreate({"model", "v1", 1}, [&] {
          ++created;
          return std::make_shared<int>(42);
        });
      });
    }
    start.set_value();
    for (auto& worker : workers)
      worker.join();
    CHECK(created == 1);
    CHECK(cache.size() == 1);
    for (const auto& session : sessions)
      CHECK(session == sessions.front());
  }

  SUBCASE("已有会话在另一个模型加载期间仍可命中") {
    auto active = cache.getOrCreate({"cached", "v1", 1}, [] { return std::make_shared<int>(1); });
    std::promise<void> entered;
    std::promise<void> release;
    const auto released = release.get_future().share();
    auto loading = std::async(std::launch::async, [&] {
      return cache.getOrCreate({"loading", "v1", 1}, [&] {
        entered.set_value();
        released.wait();
        return std::make_shared<int>(2);
      });
    });
    entered.get_future().wait();
    auto hit = std::async(std::launch::async, [&] {
      return cache.getOrCreate({"cached", "v1", 1}, [] { return std::make_shared<int>(0); });
    });
    const auto status = hit.wait_for(std::chrono::seconds(2));
    release.set_value();
    CHECK(status == std::future_status::ready);
    CHECK(hit.get() == active);
    CHECK(*loading.get() == 2);
    CHECK(cache.size() == 2);
  }
}
