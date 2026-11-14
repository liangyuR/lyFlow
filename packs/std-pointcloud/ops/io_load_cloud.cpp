#include <pcl/io/ply_io.h>

#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "lyflow_pcl/adapter.h"
#include "lyflow_pcl/pcd_ascii.h"
#include "lyflow_pcl/pcl_path.h"
#include "p1_common.h"

namespace lyflow::ops {
namespace {
using namespace p1;
struct Triangle {
  Vec3 a, b, c, normal;
  double cumulative = 0;
};

struct ScalarField {
  char kind;
  std::size_t bytes;
};

bool asciiScalar(std::string token, const ScalarField& field) {
  if (field.kind == 'F') {
    std::istringstream text(token);
    double value;
    if (!(text >> value) || !std::isfinite(value))
      return false;
    text >> std::ws;
    return text.eof() && (field.bytes == 8 || std::abs(value) <= std::numeric_limits<float>::max());
  }
  if (!token.empty() && token.front() == '+')
    token.erase(token.begin());
  if (token.empty())
    return false;
  const char* end = token.data() + token.size();
  if (field.kind == 'U') {
    std::uint64_t value;
    const auto parsed = std::from_chars(token.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
           (field.bytes == 8 || value < (std::uint64_t(1) << (field.bytes * 8)));
  }
  std::int64_t value;
  const auto parsed = std::from_chars(token.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    return false;
  if (field.bytes == 8)
    return true;
  const auto limit = std::int64_t(1) << (field.bytes * 8 - 1);
  return value >= -limit && value < limit;
}

bool preflight(const std::filesystem::path& path, bool ply, std::size_t maximum, std::string& why) {
  std::ifstream file(path, std::ios::binary);
  std::string line;
  std::size_t bytes = 0;
  std::uint64_t count = 0, width = 0, height = 1;
  std::size_t columns = 0, pointBytes = 0;
  std::vector<ScalarField> fields;
  std::vector<std::size_t> sizes;
  std::vector<char> types;
  std::size_t counts = 0;
  bool hasCounts = false;
  bool ended = false, ascii = false, vertices = true;
  std::string dataMode;
  while (std::getline(file, line)) {
    bytes += line.size() + 1;
    if (bytes > 65536) {
      why = "文件头超过64KiB";
      return false;
    }
    std::istringstream row(line);
    std::string key;
    row >> key;
    if (ply && key == "element") {
      std::string name;
      std::uint64_t amount = 0;
      if (!(row >> name >> amount))
        return false;
      if (name == "vertex")
        count = amount;
      else if (amount) {
        why = "PLY仅支持点顶点；非空面/边等其他元素需先显式采样";
        return false;
      }
      vertices = name == "vertex";
    } else if (ply && key == "format") {
      std::string version;
      row >> dataMode >> version;
      if ((dataMode != "ascii" && dataMode != "binary_little_endian") || version != "1.0") {
        why = "仅支持PLY 1.0 ASCII或小端二进制";
        return false;
      }
      ascii = dataMode == "ascii";
    } else if (ply && key == "property" && vertices) {
      std::string type;
      row >> type;
      if (type == "list") {
        why = "不支持点顶点的变长列表属性";
        return false;
      }
      const std::size_t fieldBytes =
          type == "char" || type == "uchar" || type == "int8" || type == "uint8"       ? 1
          : type == "short" || type == "ushort" || type == "int16" || type == "uint16" ? 2
          : type == "int" || type == "uint" || type == "int32" || type == "uint32" ||
                  type == "float" || type == "float32"
              ? 4
          : type == "double" || type == "float64" ? 8
                                                  : 0;
      if (!fieldBytes) {
        why = "PLY点属性类型不受支持";
        return false;
      }
      const bool floating =
          type == "float" || type == "float32" || type == "double" || type == "float64";
      const bool unsignedInteger = type == "uchar" || type == "ushort" || type == "uint" ||
                                   type == "uint8" || type == "uint16" || type == "uint32";
      fields.push_back({floating ? 'F' : unsignedInteger ? 'U' : 'I', fieldBytes});
      pointBytes += fieldBytes;
      ++columns;
    } else if (!ply && key == "FIELDS") {
      std::string name;
      while (row >> name)
        ++columns;
    } else if (!ply && key == "SIZE") {
      std::uint64_t size = 0;
      while (row >> size) {
        if (size != 1 && size != 2 && size != 4 && size != 8) {
          why = "PCD字段宽度不受支持";
          return false;
        }
        sizes.push_back(static_cast<std::size_t>(size));
        pointBytes += static_cast<std::size_t>(size);
      }
      if (!row.eof())
        return false;
    } else if (!ply && key == "TYPE") {
      std::string type;
      while (row >> type) {
        if (type != "F" && type != "I" && type != "U") {
          why = "PCD字段类型不受支持";
          return false;
        }
        types.push_back(type[0]);
      }
    } else if (!ply && key == "COUNT") {
      hasCounts = true;
      std::uint64_t value = 0;
      while (row >> value) {
        ++counts;
        if (value != 1) {
          why = "PCD仅支持标量字段";
          return false;
        }
      }
      if (!row.eof())
        return false;
    } else if (!ply && (key == "POINTS" || key == "WIDTH" || key == "HEIGHT")) {
      std::uint64_t amount = 0;
      if (!(row >> amount))
        return false;
      if (key == "POINTS")
        count = amount;
      else if (key == "WIDTH")
        width = amount;
      else
        height = amount;
    }
    if ((ply && key == "end_header") || (!ply && key == "DATA")) {
      if (!ply) {
        row >> dataMode;
        if (dataMode != "ascii" && dataMode != "binary" && dataMode != "binary_compressed") {
          why = "未知PCD编码";
          return false;
        }
        ascii = dataMode == "ascii";
      }
      ended = true;
      break;
    }
  }
  if (!ply) {
    if (sizes.size() != columns || types.size() != columns || (hasCounts && counts != columns)) {
      why = "PCD字段SIZE/TYPE/COUNT数量与FIELDS不一致";
      return false;
    }
    for (std::size_t i = 0; i < columns; ++i) {
      if (types[i] == 'F' && sizes[i] != 4 && sizes[i] != 8) {
        why = "PCD浮点字段仅支持4/8字节";
        return false;
      }
      fields.push_back({types[i], sizes[i]});
    }
    if (width && height <= maximum / width) {
      if (count && count != width * height) {
        why = "PCD POINTS与WIDTH/HEIGHT不一致";
        return false;
      }
      count = width * height;
    }
  }
  if (!ended || !columns || columns > 128 || !pointBytes || pointBytes > 128 || count > maximum ||
      width > maximum || height > maximum || (!ply && width && height > maximum / width)) {
    why = "点数超过上限或文件头截断";
    return false;
  }
  if (ascii) {
    if (!columns || columns > 128) {
      why = "ASCII字段数量非法";
      return false;
    }
    std::uint64_t rows = 0;
    while (std::getline(file, line)) {
      std::istringstream row(line);
      row >> std::ws;
      if (row.eof() || row.peek() == '#')
        continue;
      for (std::size_t i = 0; i < columns; ++i) {
        std::string token;
        if (!(row >> token) || !asciiScalar(token, fields[i])) {
          why = "ASCII正文缺值或不符合声明的字段类型/范围";
          return false;
        }
      }
      row >> std::ws;
      if (!row.eof() || ++rows > count) {
        why = "ASCII正文有多余字段或点数";
        return false;
      }
    }
    if (rows != count) {
      why = "ASCII正文点数与头部不一致";
      return false;
    }
  } else {
    const auto body = file.tellg();
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (body < 0 || end < body) {
      why = "二进制正文偏移非法";
      return false;
    }
    const auto available = static_cast<std::uint64_t>(end - body);
    const auto expected = count * pointBytes;
    if (!ply && dataMode == "binary_compressed") {
      file.seekg(body);
      std::array<unsigned char, 8> compressedSizes{};
      if (!file.read(reinterpret_cast<char*>(compressedSizes.data()), compressedSizes.size()))
        return false;
      const auto read = [&](int offset) {
        return std::uint32_t(compressedSizes[offset]) |
               (std::uint32_t(compressedSizes[offset + 1]) << 8) |
               (std::uint32_t(compressedSizes[offset + 2]) << 16) |
               (std::uint32_t(compressedSizes[offset + 3]) << 24);
      };
      if (available != 8ull + read(0) || read(1 * 4) != expected) {
        why = "压缩正文尺寸与字段点数不符";
        return false;
      }
    } else if (available != expected) {
      why = "二进制正文尺寸与字段点数不符";
      return false;
    }
  }
  return true;
}

bool blobSupported(const pcl::PCLPointCloud2& blob, std::size_t maximum, std::string& why) {
  const std::uint64_t count = static_cast<std::uint64_t>(blob.width) * blob.height;
  const std::set<std::string> supported{"x",        "y",        "z",   "intensity", "normal_x",
                                        "normal_y", "normal_z", "rgb", "_"};
  std::set<std::string> seen;
  if (blob.is_bigendian || count > maximum || blob.point_step > 128 ||
      blob.row_step != static_cast<std::uint64_t>(blob.point_step) * blob.width ||
      (count && (blob.point_step == 0 || count * blob.point_step > blob.data.size()))) {
    why = "仅支持小端、每点最多128字节且完整的有界数据";
    return false;
  }
  for (const auto& field : blob.fields) {
    if (!supported.count(field.name) ||
        (field.name != "_" && (field.count != 1 || !seen.insert(field.name).second))) {
      why = "不支持或重复的字段：" + field.name + "；不静默丢弃额外通道";
      return false;
    }
    if (field.name != "_") {
      const int bytes = field.datatype == 1 || field.datatype == 2   ? 1
                        : field.datatype == 3 || field.datatype == 4 ? 2
                        : field.datatype >= 5 && field.datatype <= 7 ? 4
                        : field.datatype == 8                        ? 8
                                                                     : 0;
      if (!bytes || static_cast<std::uint64_t>(field.offset) + bytes > blob.point_step ||
          (field.name == "rgb" && (bytes != 4 || (field.datatype != 6 && field.datatype != 7)))) {
        why = "字段类型或字节边界不受支持：" + field.name;
        return false;
      }
    }
  }
  const int normals =
      int(seen.count("normal_x")) + int(seen.count("normal_y")) + int(seen.count("normal_z"));
  if (!seen.count("x") || !seen.count("y") || !seen.count("z") || (normals && normals != 3)) {
    why = "坐标或法线通道不完整";
    return false;
  }
  return true;
}

std::uint32_t le32(const unsigned char* data) {
  return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
         (std::uint32_t(data[3]) << 24);
}

double binaryFloat(const unsigned char* data) {
  const auto bits = le32(data);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool addTriangle(std::vector<Triangle>& triangles, Vec3 a, Vec3 b, Vec3 c, double& area) {
  if (!a.allFinite() || !b.allFinite() || !c.allFinite() ||
      std::max({a.cwiseAbs().maxCoeff(), b.cwiseAbs().maxCoeff(), c.cwiseAbs().maxCoeff()}) >
          std::numeric_limits<float>::max())
    return false;
  const Vec3 cross = (b - a).cross(c - a);
  const double weight = cross.norm() * 0.5;
  if (weight > 0) {
    area += weight;
    triangles.push_back({a, b, c, cross.normalized(), area});
  }
  return true;
}

Status loadStl(const std::filesystem::path& path, std::uint64_t bytes, const ParamView& p,
               PointCloud& cloud, ExecContext& ctx) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 84> header{};
  file.read(reinterpret_cast<char*>(header.data()), header.size());
  const auto facets = le32(header.data() + 80);
  const bool binary = bytes >= 84 && bytes == 84 + 50ull * facets;
  file.clear();
  file.seekg(0);
  std::vector<Triangle> triangles;
  double area = 0;
  if (binary) {
    if (facets > 1000000)
      return error("STL面数超过100万", "path", "resource_limit");
    file.seekg(84);
    std::array<unsigned char, 50> facet{};
    for (std::uint32_t i = 0; i < facets; ++i) {
      if ((i & 4095) == 0 && ctx.cancelled())
        return Status::Ok();
      if (!file.read(reinterpret_cast<char*>(facet.data()), facet.size()) || facet[48] || facet[49])
        return error("STL截断或含不支持的颜色属性", "path");
      Vec3 vertices[3];
      for (int j = 0; j < 3; ++j)
        vertices[j] =
            Vec3(binaryFloat(facet.data() + 12 + j * 12), binaryFloat(facet.data() + 16 + j * 12),
                 binaryFloat(facet.data() + 20 + j * 12));
      if (!addTriangle(triangles, vertices[0], vertices[1], vertices[2], area))
        return error("STL顶点非有限或超出原生范围", "path");
    }
  } else {
    std::string word, line;
    if (!(file >> word) || word != "solid")
      return error("STL既非完整二进制也非ASCII solid", "path");
    std::getline(file, line);
    bool ended = false;
    std::size_t count = 0;
    while (file >> word) {
      if (word == "endsolid") {
        std::getline(file, line);
        file >> std::ws;
        ended = file.eof();
        break;
      }
      Vec3 normal, v[3];
      if (word != "facet" || !(file >> word) || word != "normal" ||
          !(file >> normal.x() >> normal.y() >> normal.z()) || !(file >> word) || word != "outer" ||
          !(file >> word) || word != "loop")
        return error("ASCII STL facet语法错误", "path");
      for (auto& vertex : v)
        if (!(file >> word) || word != "vertex" ||
            !(file >> vertex.x() >> vertex.y() >> vertex.z()))
          return error("ASCII STL vertex语法错误", "path");
      if (!(file >> word) || word != "endloop" || !(file >> word) || word != "endfacet" ||
          !addTriangle(triangles, v[0], v[1], v[2], area))
        return error("ASCII STL facet不完整或坐标非法", "path");
      if (++count > 1000000)
        return error("STL面数超过100万", "path", "resource_limit");
      if ((count & 4095) == 0 && ctx.cancelled())
        return Status::Ok();
    }
    if (!ended)
      return error("ASCII STL缺少完整endsolid", "path");
  }
  if (triangles.empty() || !std::isfinite(area) || area <= 0)
    return error("STL没有非退化三角面", "path");
  const auto samples = p.integer("samples");
  if (samples < 1 || samples > p.integer("maxPoints"))
    return error("采样数必须位于[1,maxPoints]", "samples", "bad_param");
  std::mt19937_64 random(static_cast<std::uint64_t>(p.integer("seed")));
  const auto uniform = [&] { return double(random() >> 11) / 9007199254740992.0; };
  cloud.reserve(static_cast<std::size_t>(samples));
  for (std::int64_t i = 0; i < samples; ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return Status::Ok();
    const double selected = uniform() * area;
    const auto found = std::lower_bound(
        triangles.begin(), triangles.end(), selected,
        [](const Triangle& triangle, double value) { return triangle.cumulative < value; });
    const auto& triangle = found == triangles.end() ? triangles.back() : *found;
    const double u = std::sqrt(uniform()), v = uniform();
    const Vec3 value = (1 - u) * triangle.a + u * (1 - v) * triangle.b + u * v * triangle.c;
    cloud.push(static_cast<float>(value.x()), static_cast<float>(value.y()),
               static_cast<float>(value.z()));
    for (int j = 0; j < 3; ++j)
      cloud.normals.push_back(static_cast<float>(triangle.normal[j]));
  }
  return Status::Ok();
}

} // namespace

void registerIoLoadCloud(Registry& r) {
  OperatorDesc op = descriptor("io.load_cloud", "点云与三角网格采样读取",
                               "point cloud PCD PLY STL deterministic surface sampling loader");
  Param path = text("path", "文件", "");
  path.type = ParamType::Path;
  path.mode = "open";
  path.filters = {{"点云与STL", {"pcd", "ply", "stl"}}};
  op.params = {path,
               choice("unit", "文件长度单位", "m", {{"m", "米"}, {"mm", "毫米"}}),
               text("frame", "输出参考帧", "input"),
               integer("samples", "STL采样点数", 10000, 1, 1000000),
               integer("maxPoints", "最大点数", 1000000, 1, 10000000),
               integer("seed", "采样种子", 1, 0, INT32_MAX)};
  op.inputs = {{"initial", "Transform", "初始位姿", "文件坐标→输出帧，平移单位为米。", false}};
  op.outputs = {{"cloud", "PointCloud", "点云", "米制，保留受支持通道。", true},
                {"metadata", "Record", "来源", "cloud.Asset.v1，格式、帧和采样信息。", true}};
  op.doc =
      "读取有界PCD/点顶点PLY或ASCII/小端二进制STL；STL按三角面积和固定随机种子采样并生成几何法线，不持有网格拓扑。额外字段/alpha/网格PLY/颜色STL明确拒绝。先显式换算m/mm再应用刚体初姿。";
  op.externalKey = [](const ParamView& p) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(p.path("path"), ec);
    if (ec)
      return std::string{};
    const auto time = std::filesystem::last_write_time(p.path("path"), ec);
    return ec ? std::string{}
              : std::to_string(bytes) + ":" + std::to_string(time.time_since_epoch().count());
  };
  op.compute = [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
    std::error_code ec;
    const auto path = p.path("path");
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > 256ull * 1024 * 1024 || !std::filesystem::is_regular_file(path, ec))
      return error("文件不存在、不是普通文件或超过256MiB", "path", "io_error");
    if (p.integer("maxPoints") < 1 || p.integer("maxPoints") > 10000000 ||
        !frameValid(p.text("frame")) || (p.choice("unit") != "m" && p.choice("unit") != "mm"))
      return error("点数、帧或单位参数非法", "path", "bad_param");
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    PointCloud cloud;
    Status s;
    if (extension == ".stl") {
      s = loadStl(path, bytes, p, cloud, ctx);
      if (!s.ok)
        return s;
    } else if (extension == ".pcd" || extension == ".ply") {
      std::string why;
      if (!preflight(path, extension == ".ply", static_cast<std::size_t>(p.integer("maxPoints")),
                     why))
        return error("文件头非法：" + why, "path");
      io::PclPath narrow(path, false);
      if (!narrow.ok())
        return error(narrow.error(), "path", "io_error");
      pcl::PCLPointCloud2 blob;
      const int result = extension == ".ply" ? pcl::io::loadPLYFile(narrow.str(), blob)
                                             : io::loadPcd(narrow.str(), blob, ctx.threadBudget());
      if (result < 0 || !blobSupported(blob, static_cast<std::size_t>(p.integer("maxPoints")), why))
        return error("点云格式损坏或字段不受支持：" + why, "path");
      cloud = adapter::fromBlob(blob);
      if (cloud.pointCount() != static_cast<std::uint64_t>(blob.width) * blob.height)
        return error("点云正文截断或坐标字段无效", "path");
    } else {
      return error("仅支持.pcd/.ply/.stl扩展名", "path");
    }
    if (ctx.cancelled())
      return Status::Ok();
    s = validate(cloud);
    if (!s.ok)
      return s;
    const double scale = p.choice("unit") == "mm" ? 0.001 : 1;
    for (float& value : cloud.xyz)
      value = static_cast<float>(value * scale);
    Mat4 matrix = Mat4::Identity();
    if (in.has("initial")) {
      const auto* initial = in.get("initial").asTransform();
      if (!initial || !rigid(*initial, matrix))
        return error("initial必须是文件坐标到输出帧的刚体变换", "initial");
      PointCloud aligned;
      if (!transformed(cloud, matrix, aligned, ctx))
        return ctx.cancelled() ? Status::Ok() : error("初姿变换超出范围或法线无效");
      cloud = std::move(aligned);
    }
    Record metadata{"cloud.Asset.v1",
                    {{"format", extension.substr(1)},
                     {"unit", "m"},
                     {"frame", p.text("frame")},
                     {"pointCount", cloud.pointCount()},
                     {"sampling", extension == ".stl" ? "area_weighted_seeded" : "none"}}};
    out.set("cloud", Data::cloud(std::move(cloud)));
    out.set("metadata", Data::record(std::move(metadata)));
    return Status::Ok();
  };
  r.addOperator(std::move(op));
}

} // namespace lyflow::ops
