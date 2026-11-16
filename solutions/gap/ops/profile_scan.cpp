#include "profile_migration.h"
#include "profile_scan.h"
#include "profile_guard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include <pcl/PCLPointCloud2.h>

#include "lyflow/profile.h"
#include "lyflow_pcl/adapter.h"
#include "lyflow_pcl/filters.h"
#include "lyflow_pcl/pcd_ascii.h"
#include "lyflow_pcl/pcl_path.h"
#include "lyflow_profile/merge.h"

namespace lyflow::packs::gap::profile_scene {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
Status failure(const std::string& why, const char* code = "bad_input", const char* port = "") {
  return Status::Error(Phase::Execute, code, why, {}, port);
}
Status cancelled() { return failure("场景轮廓读入已取消", "cancelled"); }
bool name(const std::string& value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos;
}
fs::path prefixFile(const fs::path& directory, const std::string& prefix, std::error_code& error) {
  std::vector<fs::path> paths;
  fs::directory_iterator entries(directory, error);
  if (error)
    return {};
  for (const auto& entry : entries) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".pcd" &&
        entry.path().filename().u8string().rfind(prefix, 0) == 0)
      paths.push_back(entry.path());
    if (error)
      return {};
  }
  std::sort(paths.begin(), paths.end());
  return paths.empty() ? fs::path{} : paths.front();
}
bool resolve(const ProfilePairOptions& options, fs::path& primary, fs::path& secondary,
             std::string& why) {
  if (options.source != "files" && options.source != "dir") {
    why = "文件来源必须显式为files或dir";
    return false;
  }
  std::error_code error;
  if (options.source == "files") {
    primary = options.primaryFile;
    secondary = options.secondaryFile;
  } else {
    if (options.dir.empty() || !name(options.primaryPrefix) || !name(options.secondaryPrefix)) {
      why = "目录来源需要明确目录与双路文件名前缀";
      return false;
    }
    primary = prefixFile(options.dir, options.primaryPrefix, error);
    if (!error)
      secondary = prefixFile(options.dir, options.secondaryPrefix, error);
  }
  if (error || primary.empty() || secondary.empty()) {
    why = "找不到双路PCD文件或无法枚举目录";
    return false;
  }
  return true;
}
bool selected(const Data& source, const std::vector<std::int32_t>& keep, Data& result,
              std::string& why, ExecContext& context) {
  const auto& input = *source.asProfile();
  Profile output;
  output.descriptor = input.descriptor;
  output.cloud = std::make_shared<const PointCloud>(input.cloud->select(keep));
  output.validity.reserve(keep.size());
  output.sourceIndices.reserve(keep.size());
  for (std::size_t i = 0; i < keep.size(); ++i) {
    if ((i & 8191U) == 0 && context.cancelled()) {
      why = "场景选槽已取消";
      return false;
    }
    const auto slot = static_cast<std::size_t>(keep[i]);
    output.validity.push_back(input.validity[slot]);
    output.sourceIndices.push_back(input.sourceIndices[slot]);
  }
  if (!validateProfile(output, why))
    return false;
  result = Data::profile(std::move(output));
  return true;
}
Status loadOne(const fs::path& path, const std::string& explicitSourceId,
               const ProfilePairOptions& options, Data& profile, Json& origin,
               ExecContext& context) {
  std::error_code error;
  if (!fs::is_regular_file(path, error))
    return failure("PCD文件不存在或不可访问：" + path.u8string(), "io");
  const auto canonical = fs::canonical(path, error);
  if (error)
    return failure("无法确定PCD实际文件路径：" + path.u8string(), "io");
  if (context.cancelled())
    return cancelled();
  ops::io::PclPath narrow(canonical, false);
  if (!narrow.ok())
    return failure(narrow.error(), "io");
  pcl::PCLPointCloud2 blob;
  if (ops::io::loadPcd(narrow.str(), blob, context.threadBudget()) < 0)
    return failure("无法解析PCD：" + canonical.u8string(), "io");
  const auto declared = static_cast<std::uint64_t>(blob.width) * blob.height;
  if (declared > std::numeric_limits<std::uint32_t>::max())
    return failure("PCD原槽数超过uint32源索引预算", "budget_exceeded");
  auto cloud = ops::adapter::fromBlob(blob);
  if (cloud.pointCount() != declared || !cloud.channelsConsistent())
    return failure("PCD缺少完整XYZ或通道/声明槽数不一致", "io");
  std::vector<std::uint8_t> validity(cloud.pointCount(), 1);
  std::vector<std::uint32_t> indices(cloud.pointCount());
  for (std::size_t i = 0; i < cloud.pointCount(); ++i) {
    if ((i & 8191U) == 0 && context.cancelled())
      return cancelled();
    indices[i] = static_cast<std::uint32_t>(i);
    for (int axis = 0; axis < 3; ++axis)
      validity[i] = validity[i] && std::isfinite(cloud.xyz[i * 3 + axis]);
  }
  const auto sourceId =
      explicitSourceId.empty() ? "file:" + canonical.u8string() : explicitSourceId;
  Profile loaded;
  std::string why;
  if (!makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                   {1, options.fileFrame, options.unit, options.layout == "profile" ? "XY" : "XZ",
                    options.handedness, "acquisition", sourceId, declared},
                   std::move(validity), std::move(indices), loaded, why))
    return failure(why, "io");
  Profile sensor;
  if (!remapProfileAxes(loaded, options.layout == "profile" ? "XZY" : "XYZ", {1, 1, 1},
                        options.sensorFrame, sensor, why,
                        [&context] { return context.cancelled(); }))
    return context.cancelled() ? cancelled() : failure(why);
  const auto size = fs::file_size(canonical, error);
  if (error)
    return failure("无法读取PCD文件大小", "io");
  const auto time = fs::last_write_time(canonical, error);
  if (error)
    return failure("无法读取PCD文件时间戳", "io");
  origin = {{"kind", "file"},
            {"sourceId", sourceId},
            {"details",
             {{"path", canonical.u8string()},
              {"fileLayout", options.layout},
              {"fileFrame", options.fileFrame},
              {"fileUnit", options.unit},
              {"fileHandedness", options.handedness},
              {"sizeBytes", size},
              {"lastWriteTicks", time.time_since_epoch().count()}}}};
  profile = Data::profile(std::move(sensor));
  return Status::Ok();
}
Json profileOrigins(const Data& primary, const Data& secondary) {
  Json result = Json::object();
  for (const auto& entry :
       {std::make_pair("primary", &primary), std::make_pair("secondary", &secondary)})
    result[entry.first] = {{"kind", "injected_profile"},
                           {"sourceId", entry.second->asProfile()->descriptor.sourceId},
                           {"details", {{"declaration", "native_profile_descriptor"}}}};
  return result;
}
ProfilePairOptions pairOptions(const ParamView& params) {
  ProfilePairOptions result;
  result.source = params.choice("source");
  result.dir = params.path("dir");
  result.primaryFile = params.path("primaryFile");
  result.secondaryFile = params.path("secondaryFile");
  result.primaryPrefix = params.text("primaryPrefix");
  result.secondaryPrefix = params.text("secondaryPrefix");
  result.layout = params.choice("layout");
  result.unit = params.choice("unit");
  result.fileFrame = params.text("fileFrame");
  result.sensorFrame = params.text("sensorFrame");
  result.handedness = params.choice("handedness");
  result.primarySourceId = params.text("primarySourceId");
  result.secondarySourceId = params.text("secondarySourceId");
  return result;
}
std::string pairKey(const ParamView& params) {
  if (params.choice("source") == "inputs")
    return {};
  fs::path primary, secondary;
  std::string why;
  if (!resolve(pairOptions(params), primary, secondary, why))
    return {};
  std::string result;
  for (const auto& path : {primary, secondary}) {
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    if (error)
      return {};
    const auto size = fs::file_size(canonical, error);
    if (error)
      return {};
    const auto stamp = fs::last_write_time(canonical, error);
    if (error)
      return {};
    result += canonical.u8string() + "|" + std::to_string(size) + "|" +
              std::to_string(stamp.time_since_epoch().count()) + "|";
  }
  return result;
}
Param text(const char* name, const char* value = "") {
  Param result;
  result.name = name;
  result.label = name;
  result.type = ParamType::String;
  result.def = Value::text(value);
  return result;
}
Param choice(const char* name, const char* value, std::initializer_list<const char*> options) {
  auto result = text(name, value);
  result.type = ParamType::Enum;
  for (const auto* option : options)
    result.options.push_back({option, option});
  return result;
}
std::vector<Param> fileParams(bool read) {
  auto source = choice("source", "dir", {"dir", "files"});
  if (read)
    source.options.push_back({"inputs", "已声明Profile输入"});
  std::vector<Param> result{source};
  for (const auto* name : {"dir", "primaryFile", "secondaryFile"}) {
    auto path = text(name);
    path.type = ParamType::Path;
    path.allowEmptyPath = read;
    path.mode = std::string(name) == "dir" ? "dir" : "open";
    path.visibleWhen.param = "source";
    path.visibleWhen.eq = Value::text(std::string(name) == "dir" ? "dir" : "files");
    result.push_back(std::move(path));
  }
  result.push_back(text("primaryPrefix", "LaserProfile_L0_Master_"));
  result.push_back(text("secondaryPrefix", "LaserProfile_R1_Slave_"));
  result.push_back(choice("layout", "sensor", {"sensor", "profile"}));
  result.push_back(choice("unit", "", {"", "m", "mm"}));
  result.push_back(text("fileFrame"));
  result.push_back(text("sensorFrame"));
  result.push_back(choice("handedness", "", {"", "right", "reflected"}));
  result.push_back(text("primarySourceId"));
  result.push_back(text("secondarySourceId"));
  return result;
}
Status loadCallback(const Inputs&, const ParamView& params, Outputs& outputs,
                    ExecContext& context) {
  LoadedProfilePair result;
  const auto status = loadProfilePair(pairOptions(params), result, context);
  if (!status.ok)
    return status;
  outputs.set("primary", std::move(result.primary));
  outputs.set("secondary", std::move(result.secondary));
  outputs.set("sourceInfo", Data::record(std::move(result.sourceInfo)));
  return Status::Ok();
}
Status readCallback(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                    ExecContext& context) {
  if (inputs.has("primary") != inputs.has("secondary"))
    return failure("primary和secondary必须同时连接或注入", "missing_input", "primary");
  LoadedProfilePair loaded;
  Json origins;
  if (inputs.has("primary")) {
    loaded.primary = inputs.get("primary");
    loaded.secondary = inputs.get("secondary");
  } else {
    if (params.choice("source") == "inputs")
      return failure("source=inputs但缺少双路已声明Profile输入", "missing_input", "primary");
    if (auto status = loadProfilePair(pairOptions(params), loaded, context); !status.ok)
      return status;
    origins = loaded.sourceInfo.data["origins"];
  }
  if (inputs.has("sourceInfo")) {
    const auto* sourceInfo = inputs.get("sourceInfo").asRecord();
    if (!sourceInfo || sourceInfo->type != "gap.ProfilePairSource.v2" ||
        !sourceInfo->data.is_object() || sourceInfo->data.size() != 2 ||
        !sourceInfo->data.contains("schemaVersion") ||
        !sourceInfo->data["schemaVersion"].is_number_integer() ||
        sourceInfo->data["schemaVersion"] != 2 || !sourceInfo->data.contains("origins"))
      return failure("需要原始配对来源gap.ProfilePairSource.v2", "bad_input", "sourceInfo");
    origins = sourceInfo->data["origins"];
  }
  ReadScanOptions options;
  options.measurementFrame = params.text("measurementFrame");
  options.removeOutliers = params.flag("removeOutliers");
  options.outlierRadiusMm = params.number("outlierRadiusMm");
  options.outlierNeighbors = params.integer("outlierNeighbors");
  options.origins = origins;
  if (inputs.has("guardQuality")) {
    const auto* guard = inputs.get("guardQuality").asRecord();
    if (!guard || guard->type != "gap.CameraGuard.v2")
      return failure("需要实际gap.CameraGuard.v2质量记录", "bad_input", "guardQuality");
    options.guard = guard->data;
    if (origins.is_object() && options.guard.is_object() && options.guard.contains("taken")) {
      const auto taken = options.guard["taken"];
      if ((taken == "primary" || taken == "secondary") &&
          origins.contains(taken.get<std::string>())) {
        const auto trusted = origins[taken.get<std::string>()];
        options.origins = {{"primary", trusted}, {"secondary", trusted}};
      }
    }
  }
  Data result;
  if (auto status = buildReadScan(loaded.primary, loaded.secondary, options, result, context);
      !status.ok)
    return status;
  outputs.set("scan", std::move(result));
  outputs.set("sensorPrimary", loaded.primary);
  outputs.set("sensorSecondary", loaded.secondary);
  return Status::Ok();
}
} // namespace

Status loadProfilePair(const ProfilePairOptions& options, LoadedProfilePair& result,
                       ExecContext& context) {
  try {
    if ((options.layout != "sensor" && options.layout != "profile") ||
        (options.unit != "m" && options.unit != "mm") || !name(options.fileFrame) ||
        !name(options.sensorFrame) ||
        (options.handedness != "right" && options.handedness != "reflected"))
      return failure("文件布局、单位、原始帧、传感器帧与手性必须明确声明", "bad_param");
    if (context.cancelled())
      return cancelled();
    fs::path primary, secondary;
    std::string why;
    if (!resolve(options, primary, secondary, why))
      return failure(why, "io");
    LoadedProfilePair candidate;
    Json primaryOrigin, secondaryOrigin;
    if (auto status = loadOne(primary, options.primarySourceId, options, candidate.primary,
                              primaryOrigin, context);
        !status.ok)
      return status;
    if (auto status = loadOne(secondary, options.secondarySourceId, options, candidate.secondary,
                              secondaryOrigin, context);
        !status.ok)
      return status;
    candidate.sourceInfo = {
        "gap.ProfilePairSource.v2",
        {{"schemaVersion", 2},
         {"origins", {{"primary", primaryOrigin}, {"secondary", secondaryOrigin}}}}};
    if (context.cancelled())
      return cancelled();
    result = std::move(candidate);
    return Status::Ok();
  } catch (const std::exception& error) {
    return failure(std::string("PCD读取失败：") + error.what(), "io");
  }
}

Status buildReadScan(const Data& sensorPrimary, const Data& sensorSecondary,
                     const ReadScanOptions& options, Data& scan, ExecContext& context) {
  const auto* primary = sensorPrimary.asProfile();
  const auto* secondary = sensorSecondary.asProfile();
  std::string why;
  if (!primary || !validateProfile(*primary, why) || !secondary ||
      !validateProfile(*secondary, why))
    return failure(why.empty() ? "双路输入必须为完整Profile，不接受缺少来源映射的裸云" : why);
  if (primary->descriptor.frame != secondary->descriptor.frame || !name(options.measurementFrame) ||
      options.measurementFrame == primary->descriptor.frame || primary->descriptor.plane != "XZ" ||
      secondary->descriptor.plane != "XZ")
    return failure("需要同传感器帧XZ轮廓与明确不同的量测目标帧");
  if (context.cancelled())
    return cancelled();
  if (!options.guard.is_null() && !guardMatches(sensorPrimary, sensorSecondary,
                                                Record{"gap.CameraGuard.v2", options.guard}, why))
    return failure(why, "bad_input", "guardQuality");
  Data frames[2], finite[2];
  const Data* sensors[2] = {&sensorPrimary, &sensorSecondary};
  for (int i = 0; i < 2; ++i) {
    Profile axes, metric;
    if (!remapProfileAxes(*sensors[i]->asProfile(), "XZY", {1, 1, 1}, options.measurementFrame,
                          axes, why, [&context] { return context.cancelled(); }) ||
        !convertProfileUnit(axes, "m", metric, why, [&context] { return context.cancelled(); }))
      return context.cancelled() ? cancelled() : failure(why);
    frames[i] = Data::profile(std::move(metric));
    std::vector<std::int32_t> keep;
    if (frames[i].asProfile()->pointCount() >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      return failure("量测轮廓超过原生Indices槽预算", "budget_exceeded");
    for (std::size_t j = 0; j < frames[i].asProfile()->pointCount(); ++j) {
      if ((j & 8191U) == 0 && context.cancelled())
        return cancelled();
      if (frames[i].asProfile()->validity[j])
        keep.push_back(static_cast<std::int32_t>(j));
    }
    if (!selected(frames[i], keep, finite[i], why, context))
      return context.cancelled() ? cancelled() : failure(why);
  }
  Record finiteProvenance[2];
  for (int i = 0; i < 2; ++i)
    if (!profile_contract::rebindProvenance(frames[i], finite[i], nullptr, finiteProvenance[i],
                                            why))
      return failure(why);
  profile_contract::MergeResult merged;
  if (!profile_contract::mergeProfiles(finite[1], finite[0], &finiteProvenance[1],
                                       &finiteProvenance[0],
                                       {"secondary", "primary", "intersection"}, merged, why,
                                       [&context] { return context.cancelled(); }))
    return context.cancelled() ? cancelled() : failure(why);
  for (const auto& channel : merged.info.data["droppedChannels"])
    context.log(LogLevel::Warn, "显式场景交集策略丢弃不共有通道：" + channel.get<std::string>());
  const auto beforeRadius = merged.profile.asProfile()->pointCount();
  if (options.removeOutliers) {
    pcl_support::RadiusSelection selection;
    if (auto status = pcl_support::radiusOutlier(*merged.profile.asProfile()->cloud,
                                                 options.outlierRadiusMm / 1000.,
                                                 options.outlierNeighbors, selection, context);
        !status.ok)
      return status;
    Data filtered;
    if (!selected(merged.profile, selection.kept.values, filtered, why, context))
      return context.cancelled() ? cancelled() : failure(why);
    Record rebound;
    if (!profile_contract::rebindProvenance(merged.profile, filtered, &merged.provenance, rebound,
                                            why))
      return failure(why);
    merged.profile = std::move(filtered);
    merged.provenance = std::move(rebound);
  }
  ScanMetadata metadata;
  metadata.sensorFrame = primary->descriptor.frame;
  metadata.measurementFrame = options.measurementFrame;
  metadata.stage = "raw";
  metadata.guard = options.guard.is_null() ? Json{{"taken", "both"},
                                                  {"evaluated", false},
                                                  {"exceeded", false},
                                                  {"onDisagree", "off"}}
                                           : options.guard;
  metadata.crop = nullptr;
  metadata.origins =
      options.origins.is_null() ? profileOrigins(sensorPrimary, sensorSecondary) : options.origins;
  metadata.processing = {
      {"method", "read"},
      {"status", "complete"},
      {"details",
       {{"mergedValidity", "compact"},
        {"merge", merged.info.data},
        {"radius",
         {{"status", options.removeOutliers ? "complete" : "skipped"},
          {"radiusMm", options.removeOutliers ? Json(options.outlierRadiusMm) : Json(nullptr)},
          {"minNeighbors", options.removeOutliers ? Json(options.outlierNeighbors) : Json(nullptr)},
          {"before", beforeRadius},
          {"after", merged.profile.asProfile()->pointCount()}}}}}};
  Data result;
  if (!makeScanPair(sensorPrimary, sensorSecondary, frames[0], frames[1], merged.profile,
                    merged.provenance, metadata, result, why))
    return failure(why);
  if (context.cancelled())
    return cancelled();
  scan = std::move(result);
  return Status::Ok();
}

void registerProfileScanOps(Registry& registry) {
  OperatorDesc load;
  load.id = "gap.load_profile_pair";
  load.version = "2.0.0";
  declareGapGraphUpgrade(load);
  load.label = "双路原生轮廓加载";
  load.category = "间隙/输入输出";
  load.keywords = {"profile pair PCD load raw slots", "双路 轮廓 读取 原槽 来源"};
  load.inputs = {};
  load.outputs = {{"primary", "Profile", "首传感器轮廓", "保留全部原槽和可选通道。", true},
                  {"secondary", "Profile", "次传感器轮廓", "软件来源声明不声称硬件认证。", true},
                  {"sourceInfo", "Record", "文件来源", "实际所选文件与原始布局声明。", true}};
  load.params = fileParams(false);
  load.capabilities.cancellable = true;
  load.capabilities.deterministic = true;
  load.externalKey = &pairKey;
  load.compute = &loadCallback;
  load.doc = "按明确布局、单位、帧和手性读入PCD；NaN槽原位保留，来源索引在任何过滤前建立。";
  registry.addOperator(std::move(load));
  OperatorDesc read;
  read.id = "gap.read_scan";
  read.version = "2.0.0";
  declareGapGraphUpgrade(read);
  read.label = "双路原生场景读入";
  read.category = "间隙/输入输出";
  read.keywords = {"profile scene read scan provenance", "双路 场景 原生 轮廓 来源"};
  read.inputs = {
      {"primary", "Profile", "首传感器轮廓", "已有完整XZ声明；两路同时连接优先于文件参数。", false},
      {"secondary", "Profile", "次传感器轮廓", "原生源槽、通道与有效性声明。", false},
      {"sourceInfo", "Record", "来源记录", "可选gap.ProfilePairSource.v2。", false},
      {"guardQuality", "Record", "guard质量", "已执行且绑定实际输入的gap.CameraGuard.v2。", false}};
  read.outputs = {
      {"scan", "Bundle<gap.ScanPair.v2>", "原生场景", "传感器原槽与完整量测/复合来源。", true},
      {"sensorPrimary", "Profile", "首传感器原槽", "共享实际选中输入。", true},
      {"sensorSecondary", "Profile", "次传感器原槽", "共享实际选中输入。", true}};
  read.params = fileParams(true);
  read.params.push_back(text("measurementFrame"));
  auto remove = text("removeOutliers");
  remove.type = ParamType::Bool;
  remove.def = Value::boolean(false);
  read.params.push_back(remove);
  auto radius = text("outlierRadiusMm");
  radius.type = ParamType::Float;
  radius.def = Value::number(.4);
  radius.unit = "mm";
  read.params.push_back(radius);
  auto neighbors = text("outlierNeighbors");
  neighbors.type = ParamType::Int;
  neighbors.def = Value::integer(5);
  read.params.push_back(neighbors);
  read.capabilities.cancellable = true;
  read.capabilities.deterministic = true;
  read.externalKey = &pairKey;
  read.compute = &readCallback;
  read.validate = [](const ParamView& params, const std::set<std::string>& connected) {
    if ((connected.count("primary") != 0) != (connected.count("secondary") != 0))
      return std::vector<Issue>{
          Issue::error("missing_input", "双路Profile必须同时连接或注入", {}, "primary")};
    if (connected.count("primary") || params.choice("source") == "inputs")
      return std::vector<Issue>{};
    std::vector<Issue> issues;
    for (const auto* field : {"dir", "primaryFile", "secondaryFile"}) {
      const bool required = params.choice("source") == "dir" ? std::string(field) == "dir"
                                                             : std::string(field) != "dir";
      if (required && params.text(field).empty())
        issues.push_back(
            Issue::error("bad_param", "未提供输入Profile时必须选择实际PCD来源路径", field));
    }
    return issues;
  };
  read.doc = "原传感器轮廓保持全槽；换轴/米制量测保原映射，仅合并测量云压缩有效槽并可选半径去噪。";
  registry.addOperator(std::move(read));
}
} // namespace lyflow::packs::gap::profile_scene
