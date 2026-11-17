#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <locale>

#include "helpers.h"
#include "lyflow/profile.h"

namespace lyflow::packs::std_profile {
void registerProfileInput(Registry&);
}

using namespace lyflow;
namespace {
struct File {
  std::filesystem::path directory;
  std::filesystem::path path;

  explicit File(const std::string& content) {
    static std::atomic<unsigned> count{0};
    directory = std::filesystem::temp_directory_path() /
                std::filesystem::u8path(
                    "lyflow-轮廓输入-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(count++));
    std::filesystem::create_directories(directory);
    path = directory / std::filesystem::u8path("测量轮廓.csv");
    write(content);
  }
  void write(const std::string& content) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    REQUIRE(file.good());
  }
  ~File() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

ParamMap params(const File& file) {
  return {{"source", Value::text("file")},
          {"path", Value::text(file.path.filename().u8string())},
          {"frame", Value::text("sensor")},
          {"unit", Value::text("mm")},
          {"plane", Value::text("XZ")},
          {"handedness", Value::text("right")},
          {"sampleOrder", Value::text("acquisition")},
          {"sourceId", Value::text("csv-source")}};
}

Registry registry() {
  Registry result;
  packs::std_profile::registerProfileInput(result);
  return result;
}

class CancelContext final : public ExecContext {
public:
  explicit CancelContext(int polls) : polls_(polls) {}
  bool cancelled() const override { return ++count_ >= polls_; }
  void progress(float, std::string_view) override {}
  void log(LogLevel, std::string) override {}
  const std::filesystem::path& baseDir() const override { return base_; }
  int threadBudget() const override { return 1; }

private:
  int polls_;
  mutable int count_ = 0;
  std::filesystem::path base_;
};
}

TEST_CASE("Profile input: strict CSV preserves NaN slots channels axes and explicit declarations") {
  const auto operators = registry();
  File file("\xEF\xBB\xBF# 采样轮廓\n\nx,z,intensity\n1,2,5\nNaN,3\n4,-NaN,7\n+5e0,-0,9\n");
  test::OpCall call(operators);
  call.base = file.directory;
  auto settings = params(file);
  REQUIRE_MESSAGE(call.run("profile.input", settings).ok, call.status.message);
  const auto data = call.out("profile");
  const auto* profile = data.asProfile();
  REQUIRE(profile);
  CHECK(profile->pointCount() == 4);
  CHECK(profile->validity == std::vector<std::uint8_t>{1, 0, 0, 1});
  CHECK(profile->sourceIndices == std::vector<std::uint32_t>{0, 1, 2, 3});
  CHECK(profile->descriptor.sourceCount == 4);
  CHECK(profile->descriptor.frame == "sensor");
  CHECK(profile->descriptor.sourceId == "csv-source");
  CHECK(profile->descriptor.unit == "mm");
  CHECK(profile->cloud->xyz[1] == 0);
  CHECK(std::isnan(profile->cloud->xyz[3]));
  CHECK(profile->cloud->xyz[5] == 3);
  CHECK(std::signbit(profile->cloud->xyz[11]));
  CHECK(profile->cloud->intensity == std::vector<float>{5, 1, 7, 9});
  std::string why;
  CHECK(validateProfile(*profile, why));
  file.write("x,y\n1000,2000\n");
  settings["plane"] = Value::text("XY");
  REQUIRE(call.run("profile.input", settings).ok);
  Profile metric;
  REQUIRE(convertProfileUnit(*call.out("profile").asProfile(), "m", metric, why));
  file.write("x,y\n1,2\n");
  settings["unit"] = Value::text("m");
  REQUIRE(call.run("profile.input", settings).ok);
  CHECK(call.out("profile").asProfile()->cloud->xyz == metric.cloud->xyz);
  CHECK(call.out("profile").asProfile()->cloud->intensity == std::vector<float>{1});
  CHECK(profile->pointCount() == 4);
  file.write("y,z\n7,8\n");
  settings["plane"] = Value::text("YZ");
  REQUIRE(call.run("profile.input", settings).ok);
  CHECK(call.out("profile").asProfile()->cloud->xyz == std::vector<float>{0, 7, 8});
}

TEST_CASE("Profile input: strict errors report physical lines without publishing partial data") {
  const auto operators = registry();
  File file("1,2\n");
  test::OpCall call(operators);
  call.base = file.directory;
  const auto settings = params(file);
  for (const std::string bad :
       {"Inf,3", "1,-infinity", "1e50,3", "1e-999,3", "1,2,NaN", "1,2,Inf", "1,2,", "1,,3",
        "1,2,3,4", "1 2", "1,2 # comment", "\"1\",2", "+-1,2", "x,z"}) {
    file.write("# comment\nx,z\n1,2\n" + bad + "\n");
    const auto status = call.run("profile.input", settings);
    INFO(bad);
    CHECK_FALSE(status.ok);
    CHECK(status.message.find("第4行") != std::string::npos);
    CHECK(call.outputs.empty());
  }
  for (const char* field : {"frame", "unit", "plane", "handedness", "sampleOrder", "sourceId"}) {
    auto missing = settings;
    missing[field] = Value::text("");
    CHECK_FALSE(call.validate("profile.input", missing, {}).empty());
  }
  file.write("# empty acquisition\n\nx,z\n");
  REQUIRE(call.run("profile.input", settings).ok);
  const auto* empty = call.out("profile").asProfile();
  REQUIRE(empty);
  CHECK(empty->pointCount() == 0);
  CHECK(empty->descriptor.sourceCount == 0);
  CHECK(empty->validity.empty());
  CHECK(empty->sourceIndices.empty());
  std::string why;
  CHECK(validateProfile(*empty, why));
}

TEST_CASE("Profile input: file point line budgets cancellation locale and external identity") {
  const auto operators = registry();
  File file("1.5,2.5\n3,4\n");
  test::OpCall call(operators);
  call.base = file.directory;
  auto settings = params(file);
  settings["maxPoints"] = Value::integer(1);
  CHECK_FALSE(call.run("profile.input", settings).ok);
  CHECK(call.outputs.empty());
  CHECK(call.status.message.find("第2行") != std::string::npos);
  settings.erase("maxPoints");
  settings["maxFileBytes"] = Value::integer(1);
  CHECK_FALSE(call.run("profile.input", settings).ok);
  CHECK(call.outputs.empty());
  settings.erase("maxFileBytes");
  file.write(std::string(65537, ' ') + "\n");
  CHECK_FALSE(call.run("profile.input", settings).ok);
  CHECK(call.status.message.find("64KiB") != std::string::npos);
  CHECK(call.outputs.empty());
  struct Comma final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  struct LocaleRestore {
    std::locale previous = std::locale();
    ~LocaleRestore() { std::locale::global(previous); }
  } restore;
  std::locale::global(std::locale(std::locale::classic(), new Comma));
  file.write("1.5,2.5\n3,4\n");
  REQUIRE(call.run("profile.input", settings).ok);
  CHECK(call.out("profile").asProfile()->cloud->xyz[0] == 1.5f);
  const auto* op = operators.find("profile.input");
  REQUIRE(op);
  ParamView parameters(call.params, call.base);
  const auto before = op->externalKey(parameters);
  const auto canonical = std::filesystem::weakly_canonical(file.path).u8string() + ":";
  INFO(before, canonical, file.directory.u8string());
  CHECK(before.rfind(canonical, 0) == 0);
  std::filesystem::create_directory(file.directory / "alias");
  auto alternate = call.params;
  alternate["path"] =
      Value::text((file.directory / "alias" / ".." / file.path.filename()).u8string());
  CHECK(op->externalKey(ParamView(alternate, call.base)) == before);
  file.write("1.5,2.5\n30,40\n");
  CHECK(op->externalKey(parameters) != before);
  for (int polls : {1, 4}) {
    CancelContext context(polls);
    call.outputs.clear();
    Inputs inputs(call.inputs);
    Outputs outputs(call.outputs);
    const auto status = op->compute(inputs, parameters, outputs, context);
    CHECK_FALSE(status.ok);
    CHECK(status.code == "cancelled");
    CHECK(call.outputs.empty());
  }
}

TEST_CASE(
    "Profile input: explicit host injection bypasses file declarations and absent payload fails") {
  const auto operators = registry();
  test::OpCall call(operators);
  CHECK(call.validate("profile.input", {}, {}).empty());
  CHECK_FALSE(call.run("profile.input").ok);
  CHECK(call.status.code == "missing_input");
  CHECK(call.status.portName == "profile");
  CHECK(call.outputs.empty());
  PointCloud cloud;
  cloud.push(3, 0, 4);
  Profile input;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "injected", "m", "XZ", "right", "unknown", "host-source", 8}, {0}, {7},
                      input, why));
  const auto data = Data::profile(input);
  const auto graph = test::makeGraph({{"source", "profile.input"}}, {});
  test::Session run(graph, {}, {}, false, 0, false, {{"source", "profile", data}});
  const auto result = run.wait();
  INFO(result.events);
  CHECK(result.runStatus() == "ok");
}
