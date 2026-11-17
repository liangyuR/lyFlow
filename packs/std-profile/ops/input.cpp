#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>

#include "lyflow/operator.h"
#include "lyflow/profile.h"
#include "lyflow/registry.h"

namespace lyflow::packs::std_profile {
namespace {
constexpr std::size_t maxLineBytes = 65536;
constexpr std::size_t maxLines = 4000000;
constexpr std::int64_t maxAllowedBytes = 1024LL * 1024 * 1024;
constexpr std::int64_t maxAllowedPoints = 16000000;

Status cancelled() { return Status::Error(Phase::Execute, "cancelled", "轮廓读取已取消"); }

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\v\f");
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(" \t\r\v\f") - first + 1);
}

std::string asciiLower(std::string_view text) {
  std::string result(text);
  for (char& value : result)
    if (value >= 'A' && value <= 'Z')
      value = static_cast<char>(value - 'A' + 'a');
  return result;
}

ProfileDescriptor descriptor(const ParamView& p) {
  ProfileDescriptor result;
  result.frame = p.text("frame");
  result.unit = p.choice("unit");
  result.plane = p.choice("plane");
  result.handedness = p.choice("handedness");
  result.sampleOrder = p.choice("sampleOrder");
  result.sourceId = p.text("sourceId");
  return result;
}

Status parameters(const ParamView& p) {
  if (p.choice("source") == "injected")
    return Status::Ok();
  std::string why;
  if (!validateProfileDescriptor(descriptor(p), why))
    return Status::Error(Phase::Execute, "bad_param", why);
  if (p.text("path").empty() || asciiLower(p.path("path").extension().u8string()) != ".csv")
    return Status::Error(Phase::Execute, "bad_param", "文件来源必须指定CSV v1的.csv路径", "path");
  return Status::Ok();
}

bool number(std::string_view text, bool allowNan, float& result) {
  text = trim(text);
  const auto normalized = asciiLower(text);
  if (allowNan && (normalized == "nan" || normalized == "+nan" || normalized == "-nan")) {
    result =
        std::copysign(std::numeric_limits<float>::quiet_NaN(), normalized[0] == '-' ? -1.f : 1.f);
    return true;
  }
  if (!text.empty() && text[0] == '+') {
    text.remove_prefix(1);
    if (!text.empty() && (text[0] == '-' || text[0] == '+'))
      return false;
  }
  if (text.empty())
    return false;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result, std::chars_format::general);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         std::isfinite(result);
}

Status compute(const Inputs&, const ParamView& p, Outputs& out, ExecContext& ctx) {
  if (ctx.cancelled())
    return cancelled();
  if (p.choice("source") == "injected")
    return Status::Error(
        Phase::Execute, "missing_input",
        "尚未注入原生Profile；请通过SDK或--input 节点.profile=文件.lypr提供完整轮廓", {},
        "profile");
  const auto file = p.path("path");
  const auto byteBudget = static_cast<std::uintmax_t>(p.integer("maxFileBytes"));
  const auto pointBudget = static_cast<std::size_t>(p.integer("maxPoints"));
  std::error_code error;
  const auto declaredBytes = std::filesystem::file_size(file, error);
  if (error || !std::filesystem::is_regular_file(file, error) || error)
    return Status::Error(Phase::Execute, "io", "无法读取轮廓CSV文件: " + file.u8string(), "path");
  if (declaredBytes > byteBudget)
    return Status::Error(Phase::Execute, "bad_input", "轮廓CSV超过文件字节预算", "maxFileBytes");
  std::ifstream input(file, std::ios::binary);
  if (!input)
    return Status::Error(Phase::Execute, "io", "无法打开轮廓CSV文件: " + file.u8string(), "path");
  ProfileDescriptor description = descriptor(p);
  const int firstAxis = description.plane == "YZ" ? 1 : 0;
  const int secondAxis = description.plane == "XY" ? 1 : 2;
  const std::array<std::string, 2> header{asciiLower(description.plane.substr(0, 1)),
                                          asciiLower(description.plane.substr(1, 1))};
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> indices;
  std::size_t lineNumber = 0;
  bool firstRecord = true;
  auto badLine = [&](const std::string& why) {
    return Status::Error(Phase::Execute, "bad_input",
                         "CSV第" + std::to_string(lineNumber) + "行: " + why, "path");
  };
  auto parseLine = [&](std::string_view line) {
    ++lineNumber;
    if (ctx.cancelled())
      return cancelled();
    if (lineNumber > maxLines)
      return badLine("超过400万物理行预算");
    if (lineNumber == 1 && line.substr(0, 3) == "\xEF\xBB\xBF")
      line.remove_prefix(3);
    line = trim(line);
    if (line.empty() || line.front() == '#')
      return Status::Ok();
    std::array<std::string_view, 3> fields;
    std::size_t count = 0;
    while (true) {
      if (count == fields.size())
        return badLine("必须为所声明平面的两个坐标及可选强度，共2或3列");
      const auto comma = line.find(',');
      fields[count++] = trim(line.substr(0, comma));
      if (comma == std::string_view::npos)
        break;
      line.remove_prefix(comma + 1);
    }
    if (count < 2)
      return badLine("必须使用逗号分隔两个坐标及可选强度");
    if (firstRecord && fields[0] == header[0] && fields[1] == header[1] &&
        (count == 2 || fields[2] == "intensity")) {
      firstRecord = false;
      return Status::Ok();
    }
    firstRecord = false;
    float first, second, intensity = 1;
    if (!number(fields[0], true, first) || !number(fields[1], true, second))
      return badLine("坐标必须为float32有限数或NaN，不允许Inf、溢出、空字段或其他文本");
    if (count == 3 && !number(fields[2], false, intensity))
      return badLine("显式强度必须为float32有限数；缺省强度请省略第三列");
    if (indices.size() >= pointBudget)
      return badLine("超过轮廓槽数预算maxPoints");
    std::array<float, 3> point{0, 0, 0};
    point[firstAxis] = first;
    point[secondAxis] = second;
    cloud.push(point[0], point[1], point[2]);
    cloud.intensity.push_back(intensity);
    validity.push_back(std::isfinite(first) && std::isfinite(second));
    indices.push_back(static_cast<std::uint32_t>(indices.size()));
    return Status::Ok();
  };
  std::array<char, 8192> buffer;
  std::string line;
  std::uintmax_t bytes = 0;
  while (input) {
    if (ctx.cancelled())
      return cancelled();
    input.read(buffer.data(), buffer.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    bytes += count;
    if (bytes > byteBudget)
      return Status::Error(Phase::Execute, "bad_input", "轮廓CSV读取期间超过文件字节预算",
                           "maxFileBytes");
    for (std::size_t index = 0; index < count; ++index) {
      if (buffer[index] == '\n') {
        const auto status = parseLine(line);
        if (!status.ok)
          return status;
        line.clear();
      } else {
        if (line.size() >= maxLineBytes) {
          ++lineNumber;
          return badLine("单行超过64KiB预算");
        }
        line.push_back(buffer[index]);
      }
    }
  }
  if (input.bad())
    return Status::Error(Phase::Execute, "io", "读取轮廓CSV失败: " + file.u8string(), "path");
  if (!line.empty()) {
    const auto status = parseLine(line);
    if (!status.ok)
      return status;
  }
  description.sourceCount = indices.size();
  if (ctx.cancelled())
    return cancelled();
  out.set("profile",
          Data::profile(Profile{std::make_shared<const PointCloud>(std::move(cloud)),
                                std::move(description), std::move(validity), std::move(indices)}));
  return Status::Ok();
}

std::string externalKey(const ParamView& p) {
  if (p.choice("source") != "file")
    return {};
  std::error_code error;
  const auto path = std::filesystem::weakly_canonical(p.path("path"), error);
  if (error)
    return {};
  const auto size = std::filesystem::file_size(path, error);
  if (error)
    return {};
  const auto time = std::filesystem::last_write_time(path, error);
  if (error)
    return {};
  return path.u8string() + ":" + std::to_string(size) + ":" +
         std::to_string(time.time_since_epoch().count());
}

Param text(const char* name, const char* label) {
  Param result;
  result.name = name;
  result.label = label;
  result.type = ParamType::String;
  result.def = Value::text("");
  result.visibleWhen = {"source", Value::text("file")};
  return result;
}

Param choice(const char* name, const char* label, std::initializer_list<const char*> values) {
  auto result = text(name, label);
  result.type = ParamType::Enum;
  for (const auto* value : values)
    result.options.push_back({value, *value ? value : "待声明"});
  return result;
}
} // namespace

void registerProfileInput(Registry& registry) {
  OperatorDesc op;
  op.id = "profile.input";
  op.version = "1.0.0";
  op.label = "原生轮廓输入";
  op.category = "轮廓/输入";
  op.keywords = {"profile", "CSV", "LYPR", "轮廓", "轮廓源", "注入"};
  op.doc =
      "注入模式接收SDK或CLI --input 节点.profile=文件.lypr提供的完整原生Profile，未注入时报错。"
      "文件模式只读CSV v1，必须显式声明帧、m/mm单位、轴平面、手性、采样顺序及源身份；不自动换单位或排序。"
      "CSV每行是所选平面的两个坐标[,intensity]；第三XYZ轴为0，缺省强度恒为1。"
      "可选首行精确小写表头为x,y或x,z或y,z，可追加intensity；支持首行UTF-8 BOM。"
      "空行与#整行注释不计槽；NaN坐标保留原槽并标无效，Inf、坏行、空字段和非法强度带行号报错。"
      "sourceCount取全部数据行数，sourceIndices按记录顺序从0递增；单行最多64KiB，总物理行最多400万。";
  op.outputs = {{"profile", "Profile", "轮廓", "只读完整槽、有效性及源索引。", true}};
  auto source = choice("source", "轮廓来源", {"injected", "file"});
  source.def = Value::text("injected");
  source.visibleWhen = {};
  source.options = {{"injected", "宿主注入"}, {"file", "CSV文件"}};
  auto path = text("path", "轮廓CSV路径");
  path.type = ParamType::Path;
  path.mode = "open";
  path.filters = {{"轮廓CSV v1", {"csv"}}};
  op.params = {source,
               path,
               text("frame", "坐标帧"),
               choice("unit", "坐标单位", {"", "m", "mm"}),
               choice("plane", "轴平面", {"", "XY", "XZ", "YZ"}),
               choice("handedness", "手性", {"", "right", "reflected"}),
               choice("sampleOrder", "采样顺序", {"", "unknown", "acquisition"}),
               text("sourceId", "采样来源身份")};
  for (const auto& setting : std::array<std::pair<const char*, const char*>, 2>{
           {{"maxFileBytes", "文件字节预算"}, {"maxPoints", "轮廓槽数预算"}}}) {
    auto limit = text(setting.first, setting.second);
    limit.type = ParamType::Int;
    limit.def = Value::integer(limit.name == "maxPoints" ? 1000000 : 256 * 1024 * 1024);
    limit.min = 1;
    limit.max = static_cast<double>(limit.name == "maxPoints" ? maxAllowedPoints : maxAllowedBytes);
    limit.advanced = true;
    op.params.push_back(std::move(limit));
  }
  op.capabilities = {true, false, true};
  op.compute = &compute;
  op.externalKey = &externalKey;
  op.validate = [](const ParamView& params, const std::set<std::string>&) {
    const auto status = parameters(params);
    if (status.ok)
      return std::vector<Issue>{};
    return std::vector<Issue>{
        Issue::error(status.code, status.message, status.paramPath, status.portName)};
  };
  registry.addOperator(std::move(op));
}
} // namespace lyflow::packs::std_profile
