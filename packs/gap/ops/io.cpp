// 读点云：一个测点目录下的 Master/Slave 两片 PCD，以及模板对。
#include <algorithm>
#include <filesystem>

#include "gap_detection/Converter.hpp"
#include "gap_ops.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;

/// 目录里第一个以 prefix 开头的 .pcd。按文件名排序取第一个，同一目录里
/// 有多次采集时结果才是确定的。
fs::path findByPrefix(const fs::path& dir, const std::string& prefix) {
  std::vector<fs::path> hits;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".pcd") {
      hits.push_back(entry.path());
    }
  }
  std::sort(hits.begin(), hits.end());
  return hits.empty() ? fs::path{} : hits.front();
}

/// 指定了文件就用文件，否则在目录里按前缀找。给脚本用的是前者（清单里写死了路径），
/// 手在界面上连图用的是后者。
fs::path resolveProfile(const ParamView& params, const char* fileParam, const char* prefixParam) {
  if (params.choice("source") == "files") return params.path(fileParam);
  const fs::path dir = params.path("dir");
  if (dir.empty()) return {};
  return findByPrefix(dir, params.text(prefixParam));
}

std::string profilePairKey(const ParamView& params) {
  const fs::path a = resolveProfile(params, "primaryFile", "primaryPrefix");
  const fs::path b = resolveProfile(params, "secondaryFile", "secondaryPrefix");
  if (a.empty() || b.empty()) return {};
  return fileStamp(a) + "|" + fileStamp(b);
}

Status loadPair(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  const bool drop = params.flag("dropNonFinite");

  struct Side {
    const char* fileParam;
    const char* prefixParam;
    const char* port;
  };
  const Side sides[2] = {{"primaryFile", "primaryPrefix", "primary"},
                         {"secondaryFile", "secondaryPrefix", "secondary"}};
  for (const Side& side : sides) {
    const fs::path file = resolveProfile(params, side.fileParam, side.prefixParam);
    if (file.empty()) {
      return Status::Error(Phase::Execute, "io",
                           std::string("找不到 ") + side.port +
                               " 的 PCD：给一个文件路径，或者给目录 + 前缀",
                           side.fileParam);
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
      return Status::Error(Phase::Execute, "io", "文件不存在: " + file.string(), side.fileParam);
    }
    GapCloud cloud;
    std::string message;
    if (!loadPcd(file.string(), &cloud, &message)) {
      return Status::Error(Phase::Execute, "io", message, side.fileParam);
    }
    if (drop) removeNonFinite(&cloud);
    outputs.set(side.port, Data::cloud(fromPcl(cloud)));
  }
  return Status::Ok();
}

Status toMeasurementFrame(const Inputs& inputs, const ParamView&, Outputs& outputs, ExecContext&) {
  const GapCloud in = toPcl(*inputs.get("cloud").asCloud());
  GapCloud out;
  // 反射而不是旋转：传感器 XZ 帧的 y 恒为 0，换轴之后 y 才是高度。
  conv::swapCloudAxis(in, &out, conv::kY, conv::kZ);
  outputs.set("cloud", Data::cloud(fromPcl(out)));
  return Status::Ok();
}

std::string templateKey(const ParamView& params) {
  const fs::path dir = params.path("dir");
  if (dir.empty()) return {};
  return fileStamp(dir / params.text("left")) + "|" + fileStamp(dir / params.text("right"));
}

Status loadTemplate(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  const fs::path dir = params.path("dir");
  if (dir.empty()) {
    return Status::Error(Phase::Execute, "bad_param", "没有给模板目录", "dir");
  }
  const struct {
    const char* param;
    const char* port;
  } sides[2] = {{"left", "left"}, {"right", "right"}};
  for (const auto& side : sides) {
    const fs::path file = dir / params.text(side.param);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
      return Status::Error(Phase::Execute, "io", "模板文件不存在: " + file.string(), side.param);
    }
    GapCloud cloud;
    std::string message;
    if (!loadPcd(file.string(), &cloud, &message)) {
      return Status::Error(Phase::Execute, "io", message, side.param);
    }
    removeNonFinite(&cloud);
    if (cloud.empty()) {
      return Status::Error(Phase::Execute, "io", "模板是空的: " + file.string(), side.param);
    }
    outputs.set(side.port, Data::cloud(fromPcl(cloud)));
  }
  return Status::Ok();
}

Param pathParam(const char* name, const char* label, const char* mode, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Path;
  p.label = label;
  p.doc = doc;
  p.def = Value::text("");
  p.mode = mode;
  return p;
}

Param textParam(const char* name, const char* label, const char* def, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::String;
  p.label = label;
  p.doc = doc;
  p.def = Value::text(def);
  return p;
}

}  // namespace

void registerLoadProfilePair(Registry& r) {
  OperatorDesc op;
  op.id = "gap.load_profile_pair";
  op.version = "1.0.0";
  op.label = "Load Profile Pair";
  op.category = "Gap/IO";
  op.keywords = {"pcd", "profile", "master", "slave", "线扫", "测点"};
  op.doc =
      "读一个测点目录下的双头线扫剖面：Master 是 primary，Slave 是 secondary。"
      "坐标是传感器 XZ 帧的米，y 恒为 0。";
  op.outputs = {
      Port{"primary", "PointCloud", "Primary", "Master（L0）那一片。", true},
      Port{"secondary", "PointCloud", "Secondary", "Slave（R1）那一片。", true},
  };

  Param drop;
  drop.name = "dropNonFinite";
  drop.type = ParamType::Bool;
  drop.label = "Drop Non-finite";
  drop.doc = "剔除 NaN 槽。对应 NonFinitePointPolicy::kRemove，与基线一致。";
  drop.def = Value::boolean(true);

  Param source;
  source.name = "source";
  source.type = ParamType::Enum;
  source.label = "Source";
  source.doc = "在目录里按前缀配对，还是直接指两个文件。";
  source.def = Value::text("dir");
  source.options = {EnumOption{"dir", "目录 + 前缀", ""}, EnumOption{"files", "指定两个文件", ""}};

  // Path 参数只要可见就是必填（plan.cpp），所以两种模式互相藏起来。
  auto onlyWhen = [](Param p, const char* value) {
    p.visibleWhen.param = "source";
    p.visibleWhen.eq = Value::text(value);
    return p;
  };

  op.params = {
      source,
      onlyWhen(pathParam("dir", "Point Dir", "dir", "测点目录，里面是两个 LaserProfile_*.pcd。"),
               "dir"),
      onlyWhen(textParam("primaryPrefix", "Primary Prefix", "LaserProfile_L0_Master_",
                         "Master 文件名前缀。"),
               "dir"),
      onlyWhen(textParam("secondaryPrefix", "Secondary Prefix", "LaserProfile_R1_Slave_",
                         "Slave 文件名前缀。"),
               "dir"),
      onlyWhen(pathParam("primaryFile", "Primary File", "open", "Master 的 PCD。"), "files"),
      onlyWhen(pathParam("secondaryFile", "Secondary File", "open", "Slave 的 PCD。"), "files"),
      drop,
  };
  op.capabilities = {false, false, true};
  op.compute = &loadPair;
  op.externalKey = &profilePairKey;
  r.addOperator(std::move(op));
}

void registerToMeasurementFrame(Registry& r) {
  OperatorDesc op;
  op.id = "gap.to_measurement_frame";
  op.version = "1.0.0";
  op.label = "To Measurement Frame";
  op.category = "Gap/Prepare";
  op.keywords = {"axis", "swap", "frame", "换轴", "测量帧"};
  op.doc = "传感器 XZ 帧 → 测量 XY 帧：交换 y 与 z。这是反射不是旋转（复刻 swapCloudAxis）。";
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "传感器帧的点云。", true}};
  op.outputs = {Port{"cloud", "PointCloud", "Cloud", "测量帧的点云。", true}};
  op.capabilities = {false, true, true};
  op.compute = &toMeasurementFrame;
  r.addOperator(std::move(op));
}

void registerLoadTemplate(Registry& r) {
  OperatorDesc op;
  op.id = "gap.load_template";
  op.version = "1.0.0";
  op.label = "Load Template";
  op.category = "Gap/IO";
  op.keywords = {"template", "模板", "pcd"};
  op.doc = "读一对模板 PCD。模板已经在测量 XY 帧里（z=0），不需要换轴。";
  op.outputs = {
      Port{"left", "PointCloud", "Left", "左模板。", true},
      Port{"right", "PointCloud", "Right", "右模板。", true},
  };
  op.params = {
      pathParam("dir", "Template Dir", "dir", "模板目录，通常是 <配置名>/。"),
      textParam("left", "Left File", "f1_left.pcd", "左模板文件名。"),
      textParam("right", "Right File", "f1_right.pcd", "右模板文件名。"),
  };
  op.capabilities = {false, false, true};
  op.compute = &loadTemplate;
  op.externalKey = &templateKey;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
