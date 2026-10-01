// 中文路径下的 PCD 往返（D9 的执行面）。pcl_path.h 的开机探测对不对，
// 只有真的写一个带中文名的文件再读回来才知道。
#include <doctest/doctest.h>

#include <pcl/io/pcd_io.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <system_error>

#include "exec/result_store.h"
#include "helpers.h"
#include "lyflow_pcl/pcd_ascii.h"

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

TEST_CASE("中文目录下 save_pcd → load_pcd 往返：中文文件名、ASCII 文件名、binary_compressed") {
  struct Case {
    const char* name;
    std::string relative;  // 相对路径 + baseDir：这正是图文件放在中文目录下时的真实形态
    const char* format;
    Json genParams;
    std::size_t points;
    int intensity;  // 1 = 读回来要有 intensity，0 = 要没有，-1 = 不看
  };
  const std::vector<Case> cases = {
      // 合成云带 intensity，PCD 里应当有 intensity 字段，读回来还在
      {"中文目录 + 中文文件名", u8"点云 输出.pcd", "binary",
       Json{{"pointCount", 3000}, {"seed", 3}}, 3000, 1},
      {"中文路径 ASCII 文件名也要能往返", "ascii-name.pcd", "ascii",
       Json{{"pointCount", 500}, {"withIntensity", false}}, 500, 0},
      {"binary_compressed 也能往返", u8"压缩.pcd", "binary_compressed",
       Json{{"pointCount", 4000}}, 4000, -1},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    ChineseTempDir dir;
    REQUIRE(std::filesystem::exists(dir.path()));

    // -- 写 -----------------------------------------------------------------
    {
      const Json doc = makeGraph(
          {
              {"g", "gen.synthetic", c.genParams},
              {"w", "io.save_pcd", Json{{"path", c.relative}, {"format", c.format}}},
          },
          {{"g.cloud", "w.cloud"}});
      const RunLog log = runGraph(doc, dir.path());
      for (const Json& e : log.ofKind("node_state")) {
        if (e.value("state", "") == "error") MESSAGE(e.dump());
      }
      CHECK(log.runStatus() == "ok");
    }

    const std::filesystem::path written = dir.path() / std::filesystem::u8path(c.relative);
    REQUIRE(std::filesystem::exists(written));
    CHECK(std::filesystem::file_size(written) > 0);

    // -- 读回来 -------------------------------------------------------------
    const Json doc = makeGraph({{"r", "io.load_pcd", Json{{"path", c.relative}}}}, {});
    Session s(doc, dir.path());
    RunLog& log = s.wait();
    for (const Json& e : log.ofKind("node_state")) {
      if (e.value("state", "") == "error") MESSAGE(e.dump());
    }
    REQUIRE(log.runStatus() == "ok");

    exec::CloudPreview p;
    REQUIRE(exec::ResultStore::instance().previewCloud(s.runId(), "r", "cloud", 0, p));
    CHECK(p.totalPoints == c.points);
    if (c.intensity >= 0) CHECK(p.hasIntensity == (c.intensity == 1));
  }
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

// ---------------------------------------------------------------------------
// ASCII PCD 的快读快写（lyflow_pcl/pcd_ascii.h）与 PCL 本身逐字节相同。每条都是同一份输入
// PCL 跑一遍、我们跑一遍，比返回值、cloud 的每个字段与每个字节、写出来的每个字节。

namespace {

namespace pio = lyflow::ops::io;

/// 纯 ASCII 名字的临时目录：窄字符串就是 .string()，PCL 与我们拿到的是同一个路径。
class AsciiTempDir {
 public:
  AsciiTempDir() {
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) / "lyflow-pcd-ascii-test";
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~AsciiTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

std::string readBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeBytes(const std::filesystem::path& p, const std::string& bytes) {
  std::ofstream f(p, std::ios::binary);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

pcl::PCLPointField field(const std::string& name, std::uint32_t offset, std::uint8_t type,
                         std::uint32_t count) {
  pcl::PCLPointField f;
  f.name = name;
  f.offset = offset;
  f.datatype = type;
  f.count = count;
  return f;
}

/// 每个字段、每个字节都一样；data 不一样时报第一个不同的字节落在第几个点。
void requireSameCloud(const pcl::PCLPointCloud2& want, const pcl::PCLPointCloud2& got) {
  CHECK(got.width == want.width);
  CHECK(got.height == want.height);
  CHECK(got.is_dense == want.is_dense);
  CHECK(got.is_bigendian == want.is_bigendian);
  CHECK(got.point_step == want.point_step);
  CHECK(got.row_step == want.row_step);
  REQUIRE(got.fields.size() == want.fields.size());
  for (std::size_t i = 0; i < want.fields.size(); ++i) {
    CAPTURE(want.fields[i].name);
    CHECK(got.fields[i].name == want.fields[i].name);
    CHECK(got.fields[i].offset == want.fields[i].offset);
    CHECK(got.fields[i].datatype == want.fields[i].datatype);
    CHECK(got.fields[i].count == want.fields[i].count);
  }
  REQUIRE(got.data.size() == want.data.size());
  const auto diff = std::mismatch(want.data.begin(), want.data.end(), got.data.begin());
  if (diff.first != want.data.end()) {
    const std::size_t at = static_cast<std::size_t>(diff.first - want.data.begin());
    FAIL_CHECK("data 第 " << at << " 字节不同（第 " << (want.point_step ? at / want.point_step : 0)
                          << " 个点）：PCL " << int(*diff.first) << "，我们 " << int(*diff.second));
  }
}

/// 同一个文件：PCL 读一遍、loadPcd 读一遍。出错时（返回 -1）PCL 留在 cloud 里的半截数据也要一样。
void requireSameRead(const std::filesystem::path& file) {
  pcl::PCLPointCloud2 want;
  pcl::PCLPointCloud2 got;
  const int rcWant = pcl::io::loadPCDFile(file.string(), want);
  const int rcGot = pio::loadPcd(file.string(), got);
  CHECK(rcGot == rcWant);
  requireSameCloud(want, got);
}

/// 同一份 cloud：PCL 写一个文件、savePcdAscii 写一个，返回值与每个字节都一样。
void requireSameWrite(const pcl::PCLPointCloud2& cloud, const AsciiTempDir& dir, const std::string& name) {
  const std::filesystem::path a = dir.file(name + ".pcl.pcd");
  const std::filesystem::path b = dir.file(name + ".ours.pcd");
  const int rcWant = pcl::io::savePCDFile(a.string(), cloud, Eigen::Vector4f::Zero(),
                                          Eigen::Quaternionf::Identity(), /*binary_mode=*/false);
  const int rcGot = pio::savePcdAscii(b.string(), cloud);
  CHECK(rcGot == rcWant);
  REQUIRE(std::filesystem::exists(b) == std::filesystem::exists(a));
  if (!std::filesystem::exists(a)) return;
  const std::string fa = readBytes(a);
  const std::string fb = readBytes(b);
  if (fa != fb) {
    const auto diff = std::mismatch(fa.begin(), fa.end(), fb.begin(), fb.end());
    const std::size_t at = static_cast<std::size_t>(diff.first - fa.begin());
    const std::size_t lineStart = at == 0 ? std::string::npos : fa.rfind('\n', at - 1);
    const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
    FAIL_CHECK("第 " << at << " 字节起不同。PCL 那一行：" << fa.substr(from, 120) << "\n我们那一行："
                     << fb.substr(from, 120));
  }
}

std::string header(const std::string& fields, const std::string& size, const std::string& type,
                   const std::string& count, unsigned width, unsigned height, unsigned points,
                   const char* eol = "\n") {
  std::string h = "# .PCD v0.7 - Point Cloud Data file format";
  h += eol;
  const std::vector<std::string> lines = {
      "VERSION 0.7",          "FIELDS " + fields,
      "SIZE " + size,         "TYPE " + type,
      "COUNT " + count,       "WIDTH " + std::to_string(width),
      "HEIGHT " + std::to_string(height), "VIEWPOINT 0 0 0 1 0 0 0",
      "POINTS " + std::to_string(points), "DATA ascii"};
  for (const std::string& line : lines) {
    h += line;
    h += eol;
  }
  return h;
}

/// 浮点的「特殊值」：PCL 写成 nan / inf / -0 / 次正规数 / 第 9 位上恰好逢五的那些。
const std::vector<float>& specialFloats() {
  static const std::vector<float> v = {
      0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 1e-45f, -1e-45f, 1.17549435e-38f, 3.40282347e38f, -3.40282347e38f,
      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(), 0.000732421875f /* 3·2^-12：第 9 位是 5 */, 16777217.0f,
      123456.789f, 1e10f, 1e-10f, 9.99999999e7f, 0.5f, 2.5f, 1.25e-5f};
  return v;
}

}  // namespace

TEST_CASE("ASCII PCD 写：与 PCL 的 savePCDFile 逐字节相同") {
  AsciiTempDir dir;
  std::mt19937 rng(20261002);

  SUBCASE("各种类型、count > 1、rgb、填充维、特殊浮点与随机位模式") {
    pcl::PCLPointCloud2 c;
    // x y z rgb _ intensity normal[3] i8 u8 i16 u16 i32 u32 d[2] _（中间一个填充维，末尾一个）
    c.fields = {field("x", 0, pcl::PCLPointField::FLOAT32, 1),
                field("y", 4, pcl::PCLPointField::FLOAT32, 1),
                field("z", 8, pcl::PCLPointField::FLOAT32, 1),
                field("rgb", 12, pcl::PCLPointField::FLOAT32, 1),
                field("_", 16, pcl::PCLPointField::FLOAT32, 1),
                field("intensity", 20, pcl::PCLPointField::FLOAT32, 1),
                field("normal", 24, pcl::PCLPointField::FLOAT32, 3),
                field("i8", 36, pcl::PCLPointField::INT8, 1),
                field("u8", 37, pcl::PCLPointField::UINT8, 1),
                field("i16", 38, pcl::PCLPointField::INT16, 1),
                field("u16", 40, pcl::PCLPointField::UINT16, 1),
                field("i32", 44, pcl::PCLPointField::INT32, 1),
                field("u32", 48, pcl::PCLPointField::UINT32, 1),
                field("d", 56, pcl::PCLPointField::FLOAT64, 2),
                field("_", 72, pcl::PCLPointField::FLOAT32, 1)};
    c.point_step = 76;
    c.width = 3000;
    c.height = 1;
    c.row_step = c.point_step * c.width;
    c.data.resize(static_cast<std::size_t>(c.row_step));
    const auto& sp = specialFloats();
    for (std::uint32_t i = 0; i < c.width; ++i) {
      std::uint8_t* p = &c.data[static_cast<std::size_t>(i) * c.point_step];
      for (int k = 0; k < 19; ++k) {  // 先整个填随机位模式：整数、rgb 与填充维都是任意值
        const std::uint32_t r = rng();
        std::memcpy(p + 4 * k, &r, 4);
      }
      // x y z intensity normal：一部分特殊值、一部分随机位模式（含 nan / inf / 次正规）、一部分普通的数
      for (const std::uint32_t off : {0u, 4u, 8u, 20u, 24u, 28u, 32u}) {
        float f;
        if (i % 3 == 0) {
          f = sp[(i / 3 + off) % sp.size()];
        } else if (i % 3 == 1) {
          const std::uint32_t r = rng();
          std::memcpy(&f, &r, 4);
        } else {
          f = std::uniform_real_distribution<float>(-100.0f, 100.0f)(rng);
        }
        std::memcpy(p + off, &f, 4);
      }
      for (const std::uint32_t off : {56u, 64u}) {
        double v;
        if (i % 2 == 0) {
          const std::uint64_t r = (static_cast<std::uint64_t>(rng()) << 32) | rng();
          std::memcpy(&v, &r, 8);
        } else {
          v = std::uniform_real_distribution<double>(-1e6, 1e6)(rng);
        }
        std::memcpy(p + off, &v, 8);
      }
    }
    requireSameWrite(c, dir, "mixed");
    // 写出来的再读回去，PCL 与我们也一样
    requireSameRead(dir.file("mixed.pcl.pcd"));
  }

  SUBCASE("有序云（height > 1）") {
    pcl::PCLPointCloud2 c;
    c.fields = {field("x", 0, pcl::PCLPointField::FLOAT32, 1), field("y", 4, pcl::PCLPointField::FLOAT32, 1)};
    c.point_step = 8;
    c.width = 3;
    c.height = 2;
    c.row_step = 24;
    c.data.resize(48);
    for (std::size_t i = 0; i < 12; ++i) {
      const float f = static_cast<float>(i) * 0.37f - 1.0f;
      std::memcpy(&c.data[i * 4], &f, 4);
    }
    requireSameWrite(c, dir, "organized");
    requireSameRead(dir.file("organized.pcl.pcd"));
  }

  SUBCASE("空云：两边都报错、都不留文件") {
    pcl::PCLPointCloud2 c;
    c.fields = {field("x", 0, pcl::PCLPointField::FLOAT32, 1)};
    c.point_step = 4;
    requireSameWrite(c, dir, "empty");
  }
}

TEST_CASE("ASCII PCD 读：与 PCL 的 loadPCDFile 逐字节相同（怪文件、怪词）") {
  AsciiTempDir dir;
  // PCL 1.12 的怪癖：第一个数走 istringstream（正确舍入成 float），之后的数都落到 atof 再截成 float
  // （先按 double 舍入一次）。这个词恰好是两种舍入结果不同的那种（1 与 1 + 2^-23 正中间往上一点点）
  const std::string tie = "1.0000000596046447753906250001";
  const std::string longDigits(70, '7');

  struct Case {
    std::string name;
    std::string text;
  };
  const std::vector<Case> cases = {
      {"CRLF、空行、只有空白的行、多余的空格与 tab",
       header("x y z", "4 4 4", "F F F", "1 1 1", 4, 1, 4, "\r\n") +
           "1 2 3\r\n\r\n  4\t5  6 \r\n   \r\n7 8 9\r\n"},
      {"特殊词：nan、inf、+、溢出、次正规、半截、十六进制、逗号、超长",
       header("a b c d e f", "4 4 4 4 4 4", "F F F F F F", "1 1 1 1 1 1", 4, 1, 4) +
           "nan NaN -nan inf -inf +1.5\n"
           "1e39 -1e39 1e-50 1e-40 .5 -.5\n"
           "5. 1e5 1E+05 0x10 1.5abc 007\n"
           "-0 0.0 1,5 " + tie + " " + longDigits + " 1\v\n"},
      {"第一个词就是双重舍入的临界值，后面再来一个",
       header("x", "4", "F", "1", 3, 1, 3) + tie + "\n" + tie + "\n0.1\n"},
      {"整数字段：越界、负数进无符号、小数、nan、带 + 号、科学计数",
       header("a b c d e f", "1 1 2 2 4 4", "I U I U I U", "1 1 1 1 1 1", 8, 1, 8) +
           "300 300 70000 70000 3000000000 5000000000\n"
           "-1 -1 -5 -5 -1 -1\n"
           "1.9 1.9 1.9 1.9 1.9 1.9\n"
           "nan nan nan nan nan nan\n"
           "127 255 32767 65535 2147483647 4294967295\n"
           "-128 0 -32768 0 -2147483648 0\n"
           "+5 +5 +5 +5 +5 +5\n"
           "1e3 1e3 1e3 1e3 -2.5 2.5\n"},
      {"double 字段：溢出、次正规、最小正规数",
       header("a b", "8 8", "F F", "1 1", 4, 1, 4) +
           "1e308 1e309\n4.9e-324 2.2250738585072014e-308\n0.1 -0\nnan inf\n"},
      {"count > 1 与填充维 _",
       header("x _ n", "4 4 4", "F F F", "1 2 3", 2, 1, 2) + "1 9 9 2 3 4\n5 9 9 6 7 8\n"},
      {"行比点少：读不够点数（报错）",
       header("x y z", "4 4 4", "F F F", "1 1 1", 5, 1, 5) + "1 2 3\n4 5 6\n7 8 9\n"},
      {"行比点多：多的不读",
       header("x y z", "4 4 4", "F F F", "1 1 1", 2, 1, 2) + "1 2 3\n4 5 6\n7 8 9\n1 1 1\n"},
      {"最后一行没有换行符", header("x y z", "4 4 4", "F F F", "1 1 1", 2, 1, 2) + "1 2 3\n4 5 6"},
      {"词数不对的行照样占一个点",
       header("x y z", "4 4 4", "F F F", "1 1 1", 3, 1, 3) + "1 2\n4 5 6\n7 8 9 10\n"},
      {"Ctrl+Z：文本模式读到它就当文件结束",
       header("x y z", "4 4 4", "F F F", "1 1 1", 3, 1, 3) + "1 2 3\n\x1a" "4 5 6\n7 8 9\n"},
      {"POINTS 0", header("x y z", "4 4 4", "F F F", "1 1 1", 0, 1, 0)},
      {"有序云", header("x y", "4 4", "F F", "1 1", 2, 2, 4) + "1 2\n3 4\n5 6\n7 8\n"},
      // 去掉首尾空白后是空串：boost::split 给一个空词（不是零个），单字段时词数刚好对上 —— 那个空词
      // 还用掉了 PCL 的流，所以紧跟着的临界值已经走 atof
      {"单字段的全空白行：一个空词，还用掉了 PCL 的流",
       header("x", "4", "F", "1", 3, 1, 3) + "   \n" + tie + "\n3\n"},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    const std::filesystem::path file = dir.file("case.pcd");
    writeBytes(file, c.text);
    requireSameRead(file);
  }

  SUBCASE("超过一块（8 MB）的文件：块边界上的半行接得上") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-50.0f, 50.0f);
    const unsigned n = 210000;
    std::string text = header("x y z intensity", "4 4 4 4", "F F F F", "1 1 1 1", n, 1, n);
    text.reserve(text.size() + std::size_t{n} * 48);
    char buf[160];
    for (unsigned i = 0; i < n; ++i) {
      const double a = u(rng), b = u(rng), c = u(rng), d = u(rng);
      const int len = std::snprintf(buf, sizeof(buf), "%.8g %.8g %.8g %.8g\n", a, b, c, d);
      text.append(buf, static_cast<std::size_t>(len));
    }
    REQUIRE(text.size() > (std::size_t{8} << 20));
    const std::filesystem::path file = dir.file("big.pcd");
    writeBytes(file, text);
    requireSameRead(file);
  }

  SUBCASE("随机的词：随机位模式按各种精度写、随机的十进制串") {
    std::mt19937 rng(42);
    const unsigned n = 40000;
    std::string text = header("f d", "4 8", "F F", "1 1", n, 1, n);
    char buf[400];
    const char* formats[] = {"%.9g", "%.8g", "%.6g", "%.17g", "%.3e", "%.12f"};
    for (unsigned i = 0; i < n; ++i) {
      std::string tok[2];
      for (int k = 0; k < 2; ++k) {
        if (i % 4 == 3) {
          // 随机的十进制串：1–25 位数字、随机小数点、随机指数
          std::string s = (rng() % 2) ? "-" : "";
          const int digits = 1 + static_cast<int>(rng() % 25);
          const int dot = static_cast<int>(rng() % static_cast<unsigned>(digits + 1));
          for (int j = 0; j < digits; ++j) {
            if (j == dot) s += '.';
            s += static_cast<char>('0' + rng() % 10);
          }
          if (rng() % 2) s += "e" + std::to_string(static_cast<int>(rng() % 90) - 45);
          tok[k] = s;
        } else {
          double v;
          if (k == 0) {
            float f;
            const std::uint32_t r = rng();
            std::memcpy(&f, &r, 4);
            v = f;
          } else {
            const std::uint64_t r = (static_cast<std::uint64_t>(rng()) << 32) | rng();
            std::memcpy(&v, &r, 8);
          }
          std::snprintf(buf, sizeof(buf), formats[rng() % 6], v);
          tok[k] = buf;
        }
      }
      text += tok[0] + " " + tok[1] + "\n";
    }
    const std::filesystem::path file = dir.file("fuzz.pcd");
    writeBytes(file, text);
    requireSameRead(file);
  }
}

TEST_CASE("小数点是逗号的 C locale：读写仍与 PCL 相同（快路径让开）") {
  const char* current = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = current ? current : "C";
  if (std::setlocale(LC_NUMERIC, "de-DE") == nullptr) {
    MESSAGE("这台机器没有 de-DE locale，跳过");
    return;
  }
  AsciiTempDir dir;
  const std::filesystem::path file = dir.file("comma.pcd");
  writeBytes(file, header("x y z", "4 4 4", "F F F", "1 1 1", 3, 1, 3) +
                       "1.5 2.25 3\n4,5 -0.125 6e2\n7 8 9.75\n");
  requireSameRead(file);

  pcl::PCLPointCloud2 c;
  c.fields = {field("x", 0, pcl::PCLPointField::FLOAT32, 1), field("y", 4, pcl::PCLPointField::FLOAT64, 1)};
  c.point_step = 12;
  c.width = 4;
  c.height = 1;
  c.row_step = 48;
  c.data.resize(48);
  for (std::size_t i = 0; i < 4; ++i) {
    const float f = 1.5f + static_cast<float>(i);
    const double d = -0.125 * static_cast<double>(i);
    std::memcpy(&c.data[i * 12], &f, 4);
    std::memcpy(&c.data[i * 12 + 4], &d, 8);
  }
  requireSameWrite(c, dir, "comma");
  std::setlocale(LC_NUMERIC, saved.c_str());
}
