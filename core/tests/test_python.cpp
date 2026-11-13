#include "helpers.h"

#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>

#include "lyflow/c_api.h"
#include "python_runtime.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {
struct PythonFixture {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("lyflow-python-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  PythonFixture() {
    std::filesystem::create_directory(dir);
    std::ofstream file(dir / "nodes.py");
    file << R"PY(
import os
import sys
import time
from pathlib import Path
import numpy as np
from lyflow_python import node, PointCloud, Image, Measurement, Record

if Path(__file__).with_name('slow.py').is_file():
    import slow

@node(id='py.integration.roundtrip', version='1.0.0', label='数据往返',
      inputs={'cloud':'PointCloud', 'image':'Image', 'measurement':'Measurement', 'record':'Record'},
      outputs={'cloud':'PointCloud', 'image':'Image', 'measurement':'Measurement', 'record':'Record'},
      params={'amount': {'type':'float', 'default':2.0},
              'curve': {'type':'curve', 'default':{'interp':'linear','points':[[0,0],[1,1]]}}})
def roundtrip(ctx, cloud, image, measurement, record, amount, curve):
    assert curve['points'][1] == [1,1]
    ctx.log('native-pipe-log')
    ctx.progress(0.5, 'native-pipe-progress')
    assert not cloud.xyz.flags.writeable
    result = cloud.copy()
    result.xyz[:, 0] += np.float32(amount)
    pixels = image.copy()
    pixels.pixels[:] += 1
    details = record.copy()
    details.data['threads'] = ctx.thread_budget
    details.data['base'] = str(ctx.base_dir)
    details.data['pid'] = os.getpid()
    details.data['requestDir'] = str(Path(cloud.xyz.filename).parent)
    print('stdout is not protocol')
    return {'cloud': result, 'image': pixels,
            'measurement':Measurement(measurement.value + amount, measurement.unit, True,
                                      'measured', 'ok', 10.0, 8.0, 12.0),
            'record':details}

@node(id='py.integration.control', version='1.0.0', label='执行控制',
      outputs={'record':'Record'}, params={'mode':{'type':'string','default':'ok'}})
def control(ctx, mode):
    print('native-stdout-' + mode, flush=True)
    print('native-stderr-' + mode, file=sys.stderr, flush=True)
    if mode == 'exception':
        raise RuntimeError('integration traceback sentinel')
    if mode == 'crash':
        os._exit(37)
    if mode == 'cooperative':
        while True:
            ctx.check_cancelled()
            time.sleep(0.01)
    if mode == 'stubborn':
        time.sleep(30)
    if mode == 'verbose':
        print('x' * (2 * 1024 * 1024), flush=True)
        print('native-verbose-end', flush=True)
    return {'record':Record({'pid':os.getpid()})}

@node(id='py.integration.invalid', version='1.0.0', label='输出边界',
      outputs={'image':'Image'}, params={'mode':{'type':'string','default':'dtype'}})
def invalid(ctx, mode):
    from lyflow_python.codec import DataCodec
    original = DataCodec.encode
    def encode(codec, value, kind):
        DataCodec.encode = original
        dto = original(codec, value, kind)
        if mode == 'dtype':
            dto['pixels']['dtype'] = 'f64'
        elif mode == 'shape':
            dto['pixels']['shape'] = [1, 2, 3]
        elif mode == 'file':
            dto['pixels']['file'] = __file__
        elif mode == 'channels':
            dto['pixels']['shape'] = [1, 1, 2]
        return dto
    DataCodec.encode = encode
    return {'image':Image(np.zeros((1, 2), dtype=np.uint8))}
)PY";
    std::ofstream bad(dir / "broken.py");
    bad << "raise ImportError('isolated broken package')\n";
  }
  ~PythonFixture() {
    python_runtime::configure(ensureRegistry(), "{}");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  Json config(int timeout = 60000) const {
    const char* executable = std::getenv("LYFLOW_TEST_PYTHON");
    return {{"executable", executable ? executable : "python"},
            {"packagePaths", {(dir / "nodes.py").u8string(), (dir / "broken.py").u8string()}},
            {"sdkDir", LYFLOW_PYTHON_SDK_DIR},
            {"timeoutMs", timeout}};
  }
};

struct PythonContext : ExecContext {
  std::atomic<bool> cancel{false};
  std::atomic<int> progressCount{0};
  std::filesystem::path base = std::filesystem::temp_directory_path();
  std::mutex mutex;
  std::vector<std::string> logs;
  bool cancelled() const override { return cancel.load(); }
  void progress(float ratio, std::string_view message) override {
    CHECK(ratio == doctest::Approx(0.5));
    CHECK(message == "native-pipe-progress");
    ++progressCount;
  }
  void log(LogLevel, std::string text) override {
    std::lock_guard<std::mutex> lock(mutex);
    logs.push_back(std::move(text));
  }
  const std::filesystem::path& baseDir() const override { return base; }
  int threadBudget() const override { return 3; }
  bool logged(std::string_view text) const {
    return std::any_of(logs.begin(), logs.end(),
                       [&](const std::string& log) { return log.find(text) != std::string::npos; });
  }
};

Status control(const ComputeFn& compute, const char* mode, PythonContext& context,
               std::unordered_map<std::string, Data>& out) {
  const ParamMap values{{"mode", Value::text(mode)}};
  const std::unordered_map<std::string, Data> input;
  Outputs output(out);
  return compute(Inputs(input), ParamView(values, context.base), output, context);
}
} // namespace

TEST_CASE("Python configuration is opt-in and dynamic registry families remain independent") {
  auto& reg = ensureRegistry();
  const auto nativeCount = reg.operators().size();
  char* raw = lyflow_python_status();
  REQUIRE(raw != nullptr);
  const auto initial = Json::parse(raw);
  lyflow_string_free(raw);
  CHECK_FALSE(initial.at("configured").get<bool>());
  CHECK(initial.at("operatorCount") == 0);
  const auto missing = python_runtime::configure(
      reg,
      R"({"executable":"C:/definitely-missing-lyflow-python.exe","packagePaths":[],"timeoutMs":1000})");
  CHECK(missing.at("configured") == true);
  CHECK(missing.at("ready") == false);
  CHECK_FALSE(missing.at("problems").empty());
  CHECK(reg.operators().size() == nativeCount);
  OpCall nativeCall;
  REQUIRE(nativeCall.run("gen.synthetic", {{"pointCount", Value::integer(2)}}).ok);
  REQUIRE(nativeCall.out("cloud").asCloud() != nullptr);
  CHECK(nativeCall.out("cloud").asCloud()->pointCount() == 2);
  CHECK(python_runtime::configure(reg, "{}").at("configured") == false);
  Registry isolated;
  OperatorDesc native, library, python;
  native.id = "native";
  library.id = "lib.test";
  python.id = "py.test.test";
  python.compute = [number = 7](const Inputs&, const ParamView&, Outputs&, ExecContext&) {
    return number == 7 ? Status::Ok() : Status::Error(Phase::Execute, "capture", "capture lost");
  };
  isolated.addOperator(native);
  isolated.setPythonOperators({python});
  isolated.setLibraryOperators({library});
  CHECK(isolated.operators().size() == 3);
  isolated.setLibraryOperators({});
  CHECK(isolated.find(python.id) != nullptr);
  isolated.setPythonOperators({});
  CHECK(isolated.find(native.id) != nullptr);
  CHECK(isolated.operators().size() == 1);
}

TEST_CASE("Python worker exchanges all supported data through a real named pipe") {
  PythonFixture fixture;
  auto& registry = ensureRegistry();
  const auto configured = python_runtime::configure(registry, fixture.config().dump());
  INFO(configured.dump());
  REQUIRE(configured.at("ready") == true);
  REQUIRE(configured.at("operatorCount") == 3);
  CHECK(configured.at("problems").size() == 1);
  CHECK_FALSE(configured.at("pythonVersion").get<std::string>().empty());
  CHECK_FALSE(configured.at("numpyVersion").get<std::string>().empty());
  const auto* op = registry.find("py.integration.roundtrip");
  REQUIRE(op != nullptr);
  CHECK_FALSE(op->capabilities.deterministic);
  CHECK_FALSE(op->capabilities.previewable);
  CHECK(op->capabilities.cancellable);
  const ComputeFn compute = op->compute;
  PointCloud cloud;
  cloud.xyz = {1, 2, 3, 4, 5, 6};
  cloud.intensity = {7, 8};
  cloud.normals = {0, 0, 1, 0, 1, 0};
  cloud.rgb = {10, 20, 30, 40, 50, 60};
  for (PixelDepth depth : {PixelDepth::U8, PixelDepth::U16, PixelDepth::F32}) {
    for (int channels : {1, 3, 4}) {
      CAPTURE(pixelDepthName(depth));
      CAPTURE(channels);
      auto image = Image::allocate(2, 3, channels, depth);
      std::unordered_map<std::string, Data> input{
          {"cloud", Data::cloud(cloud)},
          {"image", Data::image(image)},
          {"measurement", Data::measurement({5.0, true, "mm"})},
          {"record", Data::record({"integration", {{"name", "中文"}}})}};
      const ParamMap params{
          {"amount", Value::number(2.0)},
          {"curve", Value::text(R"({"interp":"linear","points":[[0,0],[1,1]]})")}};
      std::unordered_map<std::string, Data> result;
      PythonContext context;
      Outputs out(result);
      const auto status = compute(Inputs(input), ParamView(params, context.base), out, context);
      INFO(status.message);
      REQUIRE(status.ok);
      REQUIRE(result.size() == 4);
      CHECK(result.at("cloud").asCloud()->xyz == std::vector<float>{3, 2, 3, 6, 5, 6});
      CHECK(result.at("cloud").asCloud()->rgb == cloud.rgb);
      CHECK(result.at("cloud").asCloud()->normals == cloud.normals);
      CHECK(result.at("cloud").asCloud()->intensity == cloud.intensity);
      CHECK(result.at("image").asImage()->at(1, 2, channels - 1) == 1);
      CHECK(result.at("image").asImage()->depth == depth);
      CHECK(result.at("measurement").asMeasurement()->value == 7);
      CHECK(result.at("measurement").asMeasurement()->hasLimits);
      CHECK(result.at("measurement").asMeasurement()->upper == 12);
      CHECK(result.at("record").asRecord()->type == "integration");
      CHECK(result.at("record").asRecord()->data.at("name") == "中文");
      CHECK(result.at("record").asRecord()->data.at("threads") == 3);
      CHECK_FALSE(std::filesystem::exists(std::filesystem::u8path(
          result.at("record").asRecord()->data.at("requestDir").get<std::string>())));
      CHECK(context.progressCount == 1);
      CHECK(context.logged("stdout is not protocol"));
      CHECK(cloud.xyz[0] == 1);
    }
  }
  const auto refreshed = python_runtime::refresh(registry);
  CHECK(refreshed.at("ready") == true);
  CHECK(refreshed.at("operatorCount") == 3);
  const ComputeFn invalid = registry.find("py.integration.invalid")->compute;
  for (const char* mode : {"dtype", "shape", "file", "channels"}) {
    CAPTURE(mode);
    PythonContext context;
    std::unordered_map<std::string, Data> result;
    CHECK(control(invalid, mode, context, result).code == "python_output_invalid");
    CHECK(result.empty());
  }
  std::ofstream slow(fixture.dir / "slow.py");
  slow << "import time\ntime.sleep(1.0)\n";
  slow.close();
  auto refresh = std::async(std::launch::async, [&] { return python_runtime::refresh(registry); });
  for (int i = 0; i < 50 && python_runtime::status().at("ready") == true; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(refresh.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  const auto queryStart = std::chrono::steady_clock::now();
  char* status = lyflow_python_status();
  char* manifest = lyflow_manifest_json();
  const auto graph = makeGraph({}, {}).dump();
  char* validation = lyflow_validate(graph.c_str(), "");
  char* plan = lyflow_plan(graph.c_str(), "", nullptr, 0);
  CHECK(std::chrono::steady_clock::now() - queryStart < std::chrono::milliseconds(300));
  REQUIRE(status != nullptr);
  CHECK(Json::parse(status).at("ready") == false);
  CHECK(manifest != nullptr);
  CHECK(validation != nullptr);
  CHECK(plan != nullptr);
  for (char* text : {status, manifest, validation, plan})
    lyflow_string_free(text);
  CHECK(refresh.get().at("ready") == true);
}

TEST_CASE("Python failures, queued cancellation and forced termination recover on the next task") {
  PythonFixture fixture;
  auto& registry = ensureRegistry();
  const auto configured = python_runtime::configure(registry, fixture.config(3000).dump());
  INFO(configured.dump());
  REQUIRE(configured.at("ready") == true);
  const ComputeFn compute = registry.find("py.integration.control")->compute;
  PythonContext context;
  std::unordered_map<std::string, Data> result;
  const auto error = control(compute, "exception", context, result);
  CHECK(error.code == "python_exception");
  CHECK(context.logged("native-stdout-exception"));
  CHECK(context.logged("native-stderr-exception"));
  CHECK(std::any_of(context.logs.begin(), context.logs.end(), [](const std::string& text) {
    return text.find("Traceback") != std::string::npos &&
           text.find("integration traceback sentinel") != std::string::npos;
  }));
  PythonContext followingError;
  CHECK(control(compute, "ok", followingError, result).ok);
  CHECK(followingError.logged("native-stdout-ok"));
  CHECK(followingError.logged("native-stderr-ok"));
  CHECK_FALSE(followingError.logged("native-stdout-exception"));
  CHECK_FALSE(followingError.logged("native-stderr-exception"));
  PythonContext verbose;
  CHECK(control(compute, "verbose", verbose, result).ok);
  CHECK(verbose.logged("日志已截断"));
  CHECK(verbose.logged("native-verbose-end"));
  PythonContext followingVerbose;
  CHECK(control(compute, "ok", followingVerbose, result).ok);
  CHECK_FALSE(followingVerbose.logged("native-verbose-end"));
  CHECK_FALSE(followingVerbose.logged(std::string(100, 'x')));
  const auto initialPid = result.at("record").asRecord()->data.at("pid");
  for (const char* mode : {"cooperative", "stubborn"}) {
    CAPTURE(mode);
    PythonContext running;
    std::unordered_map<std::string, Data> runningResult;
    auto future = std::async(std::launch::async,
                             [&] { return control(compute, mode, running, runningResult); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    PythonContext queued;
    queued.cancel = true;
    std::unordered_map<std::string, Data> queuedResult;
    CHECK(control(compute, "ok", queued, queuedResult).code == "python_cancelled");
    const auto start = std::chrono::steady_clock::now();
    running.cancel = true;
    REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(future.get().code == "python_cancelled");
    CHECK(running.logged(std::string("native-stdout-") + mode));
    CHECK(running.logged(std::string("native-stderr-") + mode));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    PythonContext followingCancellation;
    CHECK(control(compute, "ok", followingCancellation, result).ok);
    CHECK_FALSE(followingCancellation.logged(std::string("native-stdout-") + mode));
    CHECK_FALSE(followingCancellation.logged(std::string("native-stderr-") + mode));
    CHECK(result.at("record").asRecord()->data.at("pid") != initialPid);
  }
  PythonContext crashed;
  CHECK(control(compute, "crash", crashed, result).code == "python_worker_crashed");
  CHECK(crashed.logged("native-stdout-crash"));
  CHECK(crashed.logged("native-stderr-crash"));
  const auto slow = fixture.dir / "slow.py";
  {
    std::ofstream file(slow);
    file << "import time\nimport sys\nfrom pathlib import Path\n"
            "print('native-import-stdout', flush=True)\n"
            "print('native-import-stderr', file=sys.stderr, flush=True)\n"
            "Path(__file__).with_suffix('.started').write_text('started')\n"
            "time.sleep(10)\n";
  }
  PythonContext rebuilding;
  std::unordered_map<std::string, Data> rebuildingResult;
  auto restart = std::async(std::launch::async,
                            [&] { return control(compute, "ok", rebuilding, rebuildingResult); });
  const auto marker = fixture.dir / "slow.started";
  for (int i = 0; i < 200 && !std::filesystem::exists(marker); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(std::filesystem::exists(marker));
  rebuilding.cancel = true;
  REQUIRE(restart.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(restart.get().code == "python_cancelled");
  CHECK(rebuilding.logged("native-import-stdout"));
  CHECK(rebuilding.logged("native-import-stderr"));
  std::filesystem::remove(slow);
  std::filesystem::remove(marker);
  CHECK(control(compute, "ok", context, result).ok);
  const auto beforeTimeout = std::chrono::steady_clock::now();
  PythonContext timedOut;
  CHECK(control(compute, "stubborn", timedOut, result).code == "python_timeout");
  CHECK(timedOut.logged("native-stdout-stubborn"));
  CHECK(timedOut.logged("native-stderr-stubborn"));
  CHECK(std::chrono::steady_clock::now() - beforeTimeout < std::chrono::seconds(5));
  PythonContext followingTimeout;
  CHECK(control(compute, "ok", followingTimeout, result).ok);
  CHECK_FALSE(followingTimeout.logged("native-stdout-stubborn"));
  CHECK_FALSE(followingTimeout.logged("native-stderr-stubborn"));

  const auto sourcePath = fixture.dir / "nodes.py";
  std::ifstream sourceFile(sourcePath);
  const std::string original((std::istreambuf_iterator<char>(sourceFile)),
                             std::istreambuf_iterator<char>());
  sourceFile.close();
  auto changeSource = [&](const std::string& before, const std::string& after) {
    auto changed = original;
    const auto at = changed.find(before);
    REQUIRE(at != std::string::npos);
    changed.replace(at, before.size(), after);
    std::ofstream(sourcePath) << changed;
  };
  CHECK(control(compute, "crash", context, result).code == "python_worker_crashed");
  changeSource("return {'record':Record({'pid':os.getpid()})}",
               "return {'record':Record({'pid':os.getpid(), 'body':'changed'})}");
  REQUIRE(control(compute, "ok", context, result).ok);
  CHECK(result.at("record").asRecord()->data.at("body") == "changed");
  std::ofstream(sourcePath) << original;
  REQUIRE(python_runtime::refresh(registry).at("ready") == true);
  const std::vector<std::pair<std::string, std::string>> changes{
      {"id='py.integration.control', version='1.0.0'",
       "id='py.integration.control', version='1.1.0'"},
      {"outputs={'record':'Record'}", "outputs={'record':'Measurement'}"},
      {"'mode':{'type':'string','default':'ok'}", "'mode':{'type':'string','default':'changed'}"},
      {"id='py.integration.control'", "id='py.integration.renamed'"}};
  for (const auto& change : changes) {
    CAPTURE(change.first);
    REQUIRE(control(compute, "crash", context, result).code == "python_worker_crashed");
    changeSource(change.first, change.second);
    for (int repeat = 0; repeat < 2; ++repeat) {
      const auto stale = control(compute, "ok", context, result);
      CHECK(stale.code == "python_definition_changed");
      CHECK(stale.message.find("手动刷新") != std::string::npos);
    }
    CHECK(registry.find("py.integration.control")->version == "1.0.0");
    if (change.first.find("version=") != std::string::npos) {
      REQUIRE(python_runtime::refresh(registry).at("ready") == true);
      CHECK(registry.find("py.integration.control")->version == "1.1.0");
      const auto refreshedCompute = registry.find("py.integration.control")->compute;
      CHECK(control(refreshedCompute, "ok", context, result).ok);
      CHECK(control(compute, "ok", context, result).code == "python_definition_changed");
    }
    std::ofstream(sourcePath) << original;
    REQUIRE(python_runtime::refresh(registry).at("ready") == true);
    CHECK(control(compute, "ok", context, result).ok);
  }
}
