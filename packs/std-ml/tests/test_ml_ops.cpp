// std-ml 包的算子测试。真实推理用例通过 LYFLOW_TEST_ONNX_MODEL 选择模型。
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "../ops/session_cache.h"
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
