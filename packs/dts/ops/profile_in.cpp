#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "dts_ops.h"

namespace lyflow::dts {
namespace {

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  if (params.choice("source") == "injected") {
    return Status::Error(Phase::Execute, "bad_input",
                         "来源是「宿主注入」，但这次运行没有注入轮廓", "source");
  }
  const std::filesystem::path file = params.path("path");
  std::ifstream in(file);
  if (!in) {
    return Status::Error(Phase::Execute, "io", "读不开轮廓文件: " + file.string(), "path");
  }
  PointCloud cloud;
  std::vector<float> intensity;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    for (char& c : line) {
      if (c == ',' || c == ';' || c == '\t') c = ' ';
    }
    std::istringstream ss(line);
    double x = 0, z = 0, it = 1.0;
    if (!(ss >> x >> z)) continue;
    if (!(ss >> it)) it = 1.0;
    cloud.push(static_cast<float>(x), 0.0f, static_cast<float>(z));
    intensity.push_back(static_cast<float>(it));
  }
  if (cloud.pointCount() == 0) {
    return Status::Error(Phase::Execute, "io", "轮廓文件里一个点都没有: " + file.string(), "path");
  }
  cloud.intensity = std::move(intensity);
  outputs.set("profile", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

std::string externalKey(const ParamView& params) {
  if (params.choice("source") == "injected") return {};
  const std::filesystem::path file = params.path("path");
  if (file.empty()) return {};
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" +
         std::to_string(mtime.time_since_epoch().count());
}

}  // namespace

void registerProfileIn(Registry& r) {
  OperatorDesc op;
  op.id = "dts.profile_in";
  op.version = "1.0.0";
  op.label = "轮廓输入";
  op.category = "DTS/输入";
  op.keywords = {"profile", "input", "轮廓", "输入"};
  op.doc =
      "一条激光轮廓。产线上由宿主把传感器的那一条注入进来（节点整个 compute 不跑）；\n"
      "离线调图时从 path 指的 CSV 读，每行 `x,z[,亮度]`，单位 mm，亮度 0 表示无效点。";
  op.outputs = {Port{"profile", "PointCloud", "Profile", "剖面点，y 恒为 0，x/z 单位 mm。", true}};

  Param source;
  source.name = "source";
  source.type = ParamType::Enum;
  source.label = "轮廓来源";
  source.doc = "产线与回放由宿主注入（节点整个 compute 不跑）；离线调图时读 CSV。";
  source.def = Value::text("injected");
  source.options = {EnumOption{"injected", "宿主注入", ""}, EnumOption{"file", "读 CSV 文件", ""}};

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "CSV 路径";
  path.doc = "没有注入时从这里读。相对路径按图文件所在目录解析。";
  path.def = Value::text("");
  path.mode = "open";
  path.filters = {FileFilter{"轮廓 CSV", {"csv", "txt"}}};
  path.visibleWhen.param = "source";
  path.visibleWhen.eq = Value::text("file");

  op.params = {source, path};
  op.capabilities = {false, true, true};
  op.compute = &compute;
  op.externalKey = &externalKey;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::dts
