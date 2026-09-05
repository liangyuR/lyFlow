// 中文路径下的 PCD 往返（D9 的执行面）。pcl_path.h 的开机探测对不对，
// 只有真的写一个带中文名的文件再读回来才知道。
#include <doctest/doctest.h>

#include <filesystem>
#include <system_error>

#include "exec/result_store.h"
#include "helpers.h"
#include "ops/pcl/pcl_path.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

/// 临时目录，名字里带中文和空格。析构时整个删掉。
class ChineseTempDir {
 public:
  ChineseTempDir() {
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) /
            std::filesystem::u8path(u8"lyflow 测试 中文目录");
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~ChineseTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("中文路径探测有结论") {
  // 三种模式都是可接受的结论；不可接受的是探测本身崩掉或者卡住。
  const auto mode = ops::io::narrowMode();
  CHECK((mode == ops::io::NarrowMode::Utf8 || mode == ops::io::NarrowMode::Acp ||
         mode == ops::io::NarrowMode::TempCopy));
}

TEST_CASE("中文目录 + 中文文件名：save_pcd → load_pcd 往返") {
  ChineseTempDir dir;
  REQUIRE(std::filesystem::exists(dir.path()));

  // 相对路径 + baseDir：这正是图文件放在中文目录下时的真实形态
  const std::string relative = u8"点云 输出.pcd";

  // -- 写 -------------------------------------------------------------------
  {
    const Json doc = makeGraph(
        {
            {"g", "gen.synthetic", Json{{"pointCount", 3000}, {"seed", 3}}},
            {"w", "io.save_pcd", Json{{"path", relative}, {"format", "binary"}}},
        },
        {{"g.cloud", "w.cloud"}});
    const RunLog log = runGraph(doc, dir.path());
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
    CHECK(log.runStatus() == "ok");
  }

  const std::filesystem::path written = dir.path() / std::filesystem::u8path(relative);
  REQUIRE(std::filesystem::exists(written));
  CHECK(std::filesystem::file_size(written) > 0);

  // -- 读回来 ---------------------------------------------------------------
  {
    const Json doc = makeGraph({{"r", "io.load_pcd", Json{{"path", relative}}}}, {});
    Session s(doc, dir.path());
    RunLog& log = s.wait();
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
    REQUIRE(log.runStatus() == "ok");

    exec::CloudPreview p;
    REQUIRE(exec::ResultStore::instance().previewCloud(s.runId(), "r", "cloud", 0, p));
    CHECK(p.totalPoints == 3000);
    // 合成云带 intensity，PCD 里应当有 intensity 字段，读回来还在
    CHECK(p.hasIntensity);
  }
}

TEST_CASE("中文路径 ASCII 文件名也要能往返") {
  ChineseTempDir dir;
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 500}, {"withIntensity", false}}},
          {"w", "io.save_pcd", Json{{"path", "ascii-name.pcd"}, {"format", "ascii"}}},
      },
      {{"g.cloud", "w.cloud"}});
  CHECK(runGraph(doc, dir.path()).runStatus() == "ok");
  REQUIRE(std::filesystem::exists(dir.path() / "ascii-name.pcd"));

  const Json back = makeGraph({{"r", "io.load_pcd", Json{{"path", "ascii-name.pcd"}}}}, {});
  Session s(back, dir.path());
  REQUIRE(s.wait().runStatus() == "ok");
  exec::CloudPreview p;
  REQUIRE(exec::ResultStore::instance().previewCloud(s.runId(), "r", "cloud", 0, p));
  CHECK(p.totalPoints == 500);
  CHECK_FALSE(p.hasIntensity);
}

TEST_CASE("binary_compressed 也能往返") {
  ChineseTempDir dir;
  const Json doc = makeGraph(
      {
          {"g", "gen.synthetic", Json{{"pointCount", 4000}}},
          {"w", "io.save_pcd",
           Json{{"path", u8"压缩.pcd"}, {"format", "binary_compressed"}}},
      },
      {{"g.cloud", "w.cloud"}});
  CHECK(runGraph(doc, dir.path()).runStatus() == "ok");

  const Json back = makeGraph({{"r", "io.load_pcd", Json{{"path", u8"压缩.pcd"}}}}, {});
  Session s(back, dir.path());
  REQUIRE(s.wait().runStatus() == "ok");
  exec::CloudPreview p;
  REQUIRE(exec::ResultStore::instance().previewCloud(s.runId(), "r", "cloud", 0, p));
  CHECK(p.totalPoints == 4000);
}

TEST_CASE("文件不存在时报 io 并把红框标到 path 上") {
  ChineseTempDir dir;
  const Json doc = makeGraph({{"r", "io.load_pcd", Json{{"path", u8"根本没有这个文件.pcd"}}}}, {});
  const RunLog log = runGraph(doc, dir.path());
  CHECK(log.runStatus() == "error");
  const Json e = log.nodeEvent("r", "error");
  REQUIRE(e.contains("errors"));
  CHECK(e["errors"][0]["code"] == "io");
  CHECK(e["errors"][0]["paramPath"] == "path");
}
