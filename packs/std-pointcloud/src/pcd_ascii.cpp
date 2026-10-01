// ASCII PCD 的快读快写（pcd_ascii.h）。照着 vcpkg 装的 PCL 1.12.0 的 PCDReader::read / readBodyASCII 与
// PCDWriter::writeASCII 一行行搬过来，只换掉逐个数的转换；哪里和 PCL 不一样就是 bug（tests/test_io_pcd.cpp 对照）。
//
// 读这边有个 1.12 的怪癖要照抄：detail::copyStringValue 换词时不 is.clear()，第一个数读到串尾就置上
// eofbit，之后每个数的 `is >> value` 都直接失败、落到 `static_cast<T>(atof(词))` —— 也就是先按 double
// 舍入一次、再截成 float。所以这里的快路径是 from_chars<double> 再 static_cast，不是 from_chars<float>
// （两者在极少数临界值上差 1 ulp）。atof 认 C locale 的小数点，所以小数点不是 '.' 时整个退回 PCL 的写法。
#include "lyflow_pcl/pcd_ascii.h"

#include <pcl/console/print.h>
#include <pcl/io/file_io.h>
#include <pcl/io/pcd_io.h>

#include <boost/filesystem.hpp>

#include <charconv>
#include <clocale>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace lyflow::ops::io {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

/// 快路径只收「普通十进制数」：数字开头，或 '-' / '.' 后面跟着数字（"-.5" 也算）。
/// 字母开头的（nan、inf）、'+' 开头的、太长的，一律交给 PCL —— nan 还要顺手把 is_dense 置 false。
bool plainNumber(std::string_view t) {
  if (t.empty() || t.size() > 64) return false;
  if (isDigit(t[0])) return true;
  if (t.size() < 2) return false;
  if (t[0] == '.') return isDigit(t[1]);
  if (t[0] != '-') return false;
  return isDigit(t[1]) || (t[1] == '.' && t.size() > 2 && isDigit(t[2]));
}

/// Clinger 的快路径：`[-]数字[.数字][e[±]数字]`，有效数字不超过 19 位、整数尾数 ≤ 2^53、十的幂在 ±22 以内时，
/// 尾数与 10^k 都是精确的 double，一次乘除就是正确舍入 —— 与 strtod / from_chars 逐位相同，只是快得多
/// （PCL 写出来的 %.8g 基本都落在这里）。形状不对或超出范围返回 false，交给 from_chars。
bool clingerDouble(std::string_view t, double& out) {
  static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  const char* p = t.data();
  const char* const end = p + t.size();
  const bool neg = p != end && *p == '-';
  if (neg) ++p;
  std::uint64_t m = 0;
  int significant = 0;  // 第一个非零数字起算
  int digits = 0;
  int fraction = 0;
  for (; p != end && isDigit(*p); ++p, ++digits) {
    if (significant > 0 || *p != '0') ++significant;
    if (significant > 19) return false;
    m = m * 10 + static_cast<unsigned>(*p - '0');
  }
  if (p != end && *p == '.') {
    for (++p; p != end && isDigit(*p); ++p, ++digits, ++fraction) {
      if (significant > 0 || *p != '0') ++significant;
      if (significant > 19) return false;
      m = m * 10 + static_cast<unsigned>(*p - '0');
    }
  }
  if (digits == 0) return false;
  int exp10 = 0;
  if (p != end && (*p == 'e' || *p == 'E')) {
    ++p;
    const bool eneg = p != end && *p == '-';
    if (p != end && (*p == '-' || *p == '+')) ++p;
    int e = 0;
    int edigits = 0;
    for (; p != end && isDigit(*p); ++p, ++edigits) {
      if (edigits >= 4) return false;
      e = e * 10 + (*p - '0');
    }
    if (edigits == 0) return false;  // "1e"、"1e+"：from_chars 只吃到 e 前面，交给它去判
    exp10 = eneg ? -e : e;
  }
  if (p != end) return false;
  exp10 -= fraction;
  if (m > (std::uint64_t{1} << 53)) return false;
  double v = static_cast<double>(m);
  if (m != 0) {
    if (exp10 < -22 || exp10 > 22) return false;
    v = exp10 < 0 ? v / kPow10[-exp10] : v * kPow10[exp10];
  }
  out = neg ? -v : v;
  return true;
}

/// 词 → double，与 atof 逐位相同的那部分：普通十进制数、整个词吃完、结果是正规数或 0。
/// 其余（溢出、次正规、半截）返回 false，交回 PCL。
bool plainDouble(std::string_view t, double& out) {
  if (!plainNumber(t)) return false;
  if (clingerDouble(t, out)) return true;
  const char* last = t.data() + t.size();
  const auto [ptr, ec] = std::from_chars(t.data(), last, out);
  return ec == std::errc() && ptr == last && std::fpclassify(out) != FP_SUBNORMAL;
}

bool isSpaceClassic(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/// `boost::trim(line); boost::split(st, line, boost::is_any_of("\t\r "), boost::token_compress_on)` 的
/// string_view 版：先去首尾空白；去完是空串时 boost::split 给的是一个空词（不是零个）。
void splitLine(std::vector<std::string_view>& out, std::string_view line) {
  out.clear();
  std::size_t b = 0;
  std::size_t e = line.size();
  while (b < e && isSpaceClassic(line[b])) ++b;
  while (e > b && isSpaceClassic(line[e - 1])) --e;
  line = line.substr(b, e - b);
  if (line.empty()) {
    out.emplace_back();
    return;
  }
  const char* delims = "\t\r ";
  std::size_t start = 0;
  while (true) {
    const std::size_t end = line.find_first_of(delims, start);
    if (end == std::string_view::npos) {
      out.push_back(line.substr(start));
      return;
    }
    out.push_back(line.substr(start, end - start));
    start = line.find_first_not_of(delims, end);  // 连着的分隔符算一个；去过首尾空白，后面一定还有词
  }
}

template <typename T>
void store(pcl::PCLPointCloud2& cloud, unsigned idx, unsigned d, unsigned c, T value) {
  std::memcpy(&cloud.data[static_cast<std::size_t>(idx) * cloud.point_step + cloud.fields[d].offset +
                          c * sizeof(T)],
              &value, sizeof(T));
}

/// 一个词写进 cloud。流还好着（文件里头一两个数）或词不普通：原样调 PCL 的 copyStringValue，
/// 连 `is` 的状态都跟着它变；否则就是 PCL 落到的那句 `static_cast<T>(atof(词))`。
template <typename T>
void copyToken(std::string_view tok, pcl::PCLPointCloud2& cloud, unsigned idx, unsigned d, unsigned c,
               std::istringstream& is, bool fast) {
  double v = 0;
  if (fast && !is.good() && plainDouble(tok, v)) {
    if constexpr (std::is_floating_point_v<T>) {
      if (std::is_same_v<T, double> || std::fabs(v) <= std::numeric_limits<float>::max()) {
        store(cloud, idx, d, c, static_cast<T>(v));
        return;
      }
    } else if constexpr (sizeof(T) == 1) {
      // int8 / uint8：PCL 先 static_cast<int>(atof) 再截断
      if (v > -2147483649.0 && v < 2147483648.0) {
        store(cloud, idx, d, c, static_cast<T>(static_cast<int>(v)));
        return;
      }
    } else {
      // 只收转得过去的（截断后落在 T 的范围里），越界的 static_cast 是未定义行为，交回 PCL
      if (v > static_cast<double>(std::numeric_limits<T>::lowest()) - 1.0 &&
          v < static_cast<double>(std::numeric_limits<T>::max()) + 1.0) {
        store(cloud, idx, d, c, static_cast<T>(v));
        return;
      }
    }
  }
  pcl::copyStringValue<T>(std::string(tok), cloud, static_cast<pcl::index_t>(idx), d, c, is);
}

/// readBodyASCII 的搬运。fs 已经 seek 到正文开头（文本模式打开，CRLF 由 CRT 换成 LF，与 PCL 相同）。
int readBodyAscii(std::istream& fs, pcl::PCLPointCloud2& cloud) {
  const unsigned nr_points = cloud.width * cloud.height;
  const unsigned elems_per_line =
      std::accumulate(cloud.fields.cbegin(), cloud.fields.cend(), 0u,
                      [](const auto& i, const auto& field) { return i + field.count; });
  cloud.is_dense = true;

  unsigned idx = 0;
  std::vector<std::string_view> st;
  st.reserve(elems_per_line);
  std::istringstream is;
  is.imbue(std::locale::classic());
  // atof 认的是 C locale 的小数点：不是 '.' 时不走快路径，逐个词照 PCL 来（慢，但一样）
  const lconv* lc = std::localeconv();
  const bool fast = lc != nullptr && lc->decimal_point != nullptr && std::strcmp(lc->decimal_point, ".") == 0;

  // 一行（getline 读出来的，不含 '\n'）。返回 false = 点数够了，PCL 在这里就不再往下读
  auto line = [&](std::string_view text) -> bool {
    if (text.empty()) return true;  // 空行跳过、不占点
    splitLine(st, text);
    if (st.size() != elems_per_line) {
      PCL_WARN("[pcl::PCDReader::readBodyASCII] Possibly malformed PCD file: point number %u has %zu "
               "elements, but should have %u\n",
               idx + 1, st.size(), elems_per_line);
      ++idx;  // 这一行跳过，但照样占一个点（PCL 就是这么数的）
      return idx < nr_points;
    }
    std::size_t total = 0;
    for (unsigned d = 0; d < static_cast<unsigned>(cloud.fields.size()); ++d) {
      const pcl::PCLPointField& f = cloud.fields[d];
      if (f.name == "_") {  // 二进制数据带过来的填充维
        total += f.count;
        continue;
      }
      for (pcl::uindex_t c = 0; c < f.count; ++c) {
        const std::string_view tok = st[total + c];
        switch (f.datatype) {
          case pcl::PCLPointField::INT8: copyToken<std::int8_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::UINT8: copyToken<std::uint8_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::INT16: copyToken<std::int16_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::UINT16: copyToken<std::uint16_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::INT32: copyToken<std::int32_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::UINT32: copyToken<std::uint32_t>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::FLOAT32: copyToken<float>(tok, cloud, idx, d, c, is, fast); break;
          case pcl::PCLPointField::FLOAT64: copyToken<double>(tok, cloud, idx, d, c, is, fast); break;
          default:
            PCL_WARN("[pcl::PCDReader::read] Incorrect field data type specified (%d)!\n", f.datatype);
            break;
        }
      }
      total += f.count;
    }
    ++idx;
    return idx < nr_points;
  };

  // getline 一行行地读换成整块读、自己按 '\n' 切：一块切不完的那半行留到下一块
  if (idx < nr_points) {
    std::vector<char> buf(std::size_t{8} << 20);
    std::string carry;
    bool more = true;
    while (more) {
      fs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::size_t got = static_cast<std::size_t>(fs.gcount());
      if (got == 0) break;
      const std::string_view chunk(buf.data(), got);
      std::size_t pos = 0;
      while (more) {
        const std::size_t nl = chunk.find('\n', pos);
        if (nl == std::string_view::npos) break;
        if (carry.empty()) {
          more = line(chunk.substr(pos, nl - pos));
        } else {
          carry.append(chunk.substr(pos, nl - pos));
          more = line(carry);
          carry.clear();
        }
        pos = nl + 1;
      }
      if (more) carry.append(chunk.substr(pos));
    }
    // 文件最后一行没有换行符：getline 照样把它读出来
    if (more && !carry.empty()) line(carry);
  }

  if (idx != nr_points) {
    PCL_ERROR("[pcl::PCDReader::read] Number of points read (%d) is different than expected (%d)\n", idx,
              nr_points);
    return -1;
  }
  return 0;
}

/// `stream << value`（precision 8、classic locale）：MSVC 的 num_put 把 float 提升成 double 再
/// sprintf_s("%.*g")；to_chars 的 general + precision 就是「printf 风格」，Windows 10 2004 起 UCRT 的
/// printf 也按 IEEE 舍入，两边逐字相同（tests 里拿 PCL 对照）。
void appendDouble(std::string& out, double value, int precision) {
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general, precision);
  out.append(buf, ec == std::errc() ? ptr : buf);
}

template <typename T>
void appendInt(std::string& out, T value) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  out.append(buf, ec == std::errc() ? ptr : buf);
}

template <typename T>
T load(const pcl::PCLPointCloud2& cloud, int i, int point_size, unsigned d, int c) {
  T value;
  std::memcpy(&value,
              &cloud.data[static_cast<std::size_t>(i) * point_size + cloud.fields[d].offset + c * sizeof(T)],
              sizeof(T));
  return value;
}

template <typename T>
void appendFloat(std::string& out, T value, int precision) {
  if (std::isnan(value)) {
    out += "nan";
  } else {
    appendDouble(out, static_cast<double>(value), precision);
  }
}

}  // namespace

int loadPcd(const std::string& file, pcl::PCLPointCloud2& cloud) {
  // PCDReader::read：先查文件在不在（boost 的 exists：窄字符串按 ANSI 代码页解释，与 PCL 一致），再读头
  if (file.empty() || !boost::filesystem::exists(file)) {
    PCL_ERROR("[pcl::PCDReader::read] Could not find file '%s'.\n", file.c_str());
    return -1;
  }
  pcl::PCDReader reader;
  Eigen::Vector4f origin;
  Eigen::Quaternionf orientation;
  int version = 0;
  int data_type = 0;
  unsigned data_idx = 0;
  int res = reader.readHeader(file, cloud, origin, orientation, version, data_type, data_idx, 0);
  if (res < 0) return res;
  if (data_type != 0) {
    // 两种二进制 PCL 本来就快（mmap）：整个交回去，头再读一遍不值一提
    cloud = pcl::PCLPointCloud2();
    return reader.read(file, cloud);
  }
  std::ifstream fs;
  fs.open(file.c_str());  // 文本模式，与 PCL 相同
  if (!fs.is_open() || fs.fail()) {
    PCL_ERROR("[pcl::PCDReader::read] Could not open file %s.\n", file.c_str());
    return -1;
  }
  fs.seekg(data_idx);
  res = readBodyAscii(fs, cloud);
  fs.close();
  return res < 0 ? res : 0;
}

int savePcdAscii(const std::string& file, const pcl::PCLPointCloud2& cloud) {
  constexpr int precision = 8;  // PCDWriter::write(…, binary=false) 传的就是 8
  if (cloud.data.empty()) {
    PCL_ERROR("[pcl::PCDWriter::writeASCII] Input point cloud has no data!\n");
    return -1;
  }
  std::ofstream fs;
  fs.open(file.c_str(), std::ios::binary);
  if (!fs.is_open() || fs.fail()) {
    PCL_ERROR("[pcl::PCDWriter::writeASCII] Could not open file '%s' for writing! Error : %s\n", file.c_str(),
              std::strerror(errno));
    return -1;
  }
  const int nr_points = static_cast<int>(cloud.width * cloud.height);
  // PCL 这里不判 0 就除（有数据却 width*height 为 0 的云会让它崩）：判一下，那种云只写头
  const int point_size = (nr_points == 0) ? 0 : static_cast<int>(cloud.data.size() / nr_points);

  pcl::PCDWriter writer;
  fs << writer.generateHeaderASCII(cloud, Eigen::Vector4f::Zero(), Eigen::Quaternionf::Identity())
     << "DATA ascii\n";

  std::string out;
  out.reserve(std::size_t{8} << 20);
  std::string lineText;
  const unsigned fieldCount = static_cast<unsigned>(cloud.fields.size());
  for (int i = 0; i < nr_points; ++i) {
    lineText.clear();
    for (unsigned d = 0; d < fieldCount; ++d) {
      const pcl::PCLPointField& f = cloud.fields[d];
      if (f.name == "_") continue;
      int count = static_cast<int>(f.count);
      if (count == 0) count = 1;  // PCL：「we simply cannot tolerate 0 counts」
      for (int c = 0; c < count; ++c) {
        switch (f.datatype) {
          case pcl::PCLPointField::INT8:
            appendInt(lineText, static_cast<int>(load<std::int8_t>(cloud, i, point_size, d, c)));
            break;
          case pcl::PCLPointField::UINT8:
            appendInt(lineText, static_cast<unsigned>(load<std::uint8_t>(cloud, i, point_size, d, c)));
            break;
          case pcl::PCLPointField::INT16: appendInt(lineText, load<std::int16_t>(cloud, i, point_size, d, c)); break;
          case pcl::PCLPointField::UINT16: appendInt(lineText, load<std::uint16_t>(cloud, i, point_size, d, c)); break;
          case pcl::PCLPointField::INT32: appendInt(lineText, load<std::int32_t>(cloud, i, point_size, d, c)); break;
          case pcl::PCLPointField::UINT32: appendInt(lineText, load<std::uint32_t>(cloud, i, point_size, d, c)); break;
          case pcl::PCLPointField::FLOAT32:
            // PCL：rgb 虽然是 float，按 uint32 写（好几个不透明的颜色值按 float 看是 nan）
            if (f.name == "rgb") {
              appendInt(lineText, load<std::uint32_t>(cloud, i, point_size, d, c));
            } else {
              appendFloat(lineText, load<float>(cloud, i, point_size, d, c), precision);
            }
            break;
          case pcl::PCLPointField::FLOAT64:
            appendFloat(lineText, load<double>(cloud, i, point_size, d, c), precision);
            break;
          default:
            PCL_WARN("[pcl::PCDWriter::writeASCII] Incorrect field data type specified (%d)!\n", f.datatype);
            break;
        }
        if (d < fieldCount - 1 || c < static_cast<int>(f.count) - 1) lineText += ' ';
      }
    }
    // PCL 在这里 boost::trim：值里不会有空白，要去的只是首尾的空格
    const std::size_t b = lineText.find_first_not_of(' ');
    if (b != std::string::npos) {
      const std::size_t e = lineText.find_last_not_of(' ');
      out.append(lineText, b, e - b + 1);
    }
    out += '\n';
    if (out.size() >= (std::size_t{8} << 20)) {
      fs.write(out.data(), static_cast<std::streamsize>(out.size()));
      out.clear();
    }
  }
  fs.write(out.data(), static_cast<std::streamsize>(out.size()));
  fs.close();
  return 0;
}

}  // namespace lyflow::ops::io
