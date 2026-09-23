// gap.measure_reference：整条主路径的黑盒对照（G5）。直接调 MeasurementEngine，
// 与 gap_batch_runner 走同一份代码，用来和拆分算子逐节点对拍。
#include <filesystem>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "gap_core/MeasurementEngine.hpp"
#include "gap_detection/DetectionConfigurationAdapter.hpp"
#include "gap_ml/SensorXzProfile.hpp"
#include "gap_ops.h"
#include "lyflow/json_writer.h"

namespace lyflow::packs::gap {
namespace {

namespace fs = std::filesystem;

std::string configKey(const ParamView& params) {
  const std::string config = fileStamp(params.path("configPath"));
  if (config.empty()) return {};
  return config + "|" + (params.flag("useModel") ? fileStamp(params.path("modelPath")) : "");
}

/// 复刻 BatchRunner.cpp:625 的 applyModelRoi：原始 1280 槽 → 推理 → 四框进 roi_override。
/// 返回空串表示成功，否则是失败原因。
std::string applyModelRoi(const ::gap::ml::OnnxRoiPredictor& predictor,
                          ::gap::core::MeasurementRequest& request) {
  const auto primary = request.primary_cloud.size();
  const auto secondary = request.secondary_cloud.size();
  if (primary != ::gap::ml::kProfileSlots || secondary != ::gap::ml::kProfileSlots) {
    return "模型 ROI 要的是每片正好 " + std::to_string(::gap::ml::kProfileSlots) +
           " 个点的原始剖面（拿到 primary=" + std::to_string(primary) +
           ", secondary=" + std::to_string(secondary) + "）";
  }
  ::gap::ml::RoiBoxesResult prediction;
  try {
    prediction = predictor.predict(::gap::ml::profileRowsFromSensorXzCloud(request.primary_cloud),
                                   ::gap::ml::profileRowsFromSensorXzCloud(request.secondary_cloud));
  } catch (const std::exception& e) {
    return std::string("模型 ROI 推理失败: ") + e.what();
  }
  if (!prediction.missing_segments.empty()) {
    std::string joined;
    for (const auto& segment : prediction.missing_segments) {
      if (!joined.empty()) joined += ", ";
      joined += segment;
    }
    return "模型没标出这些段: " + joined;
  }
  request.configuration.roi_override = prediction.rois;
  request.configuration.roi_override_source = "model";
  return {};
}

/// 诊断 JSON。与 diagnostics.jsonl 的字段同名，逐阶段对拍时直接能比。
std::string qualityJson(const ::gap::core::MeasurementResult& result) {
  JsonWriter w;
  w.setIndent(0);
  w.beginObject();
  w.field("roiSource", result.quality.roi_source);
  w.fieldIfSet("cropStatus", result.quality.crop_status);
  w.key("effectiveRoi");
  w.beginObject();
  for (const auto& [name, box] : result.quality.effective_roi) {
    w.key(name);
    w.beginArray();
    for (double v : box) w.value(v);
    w.endArray();
  }
  w.endObject();
  w.key("icp");
  w.beginArray();
  for (const auto& icp : result.quality.icp) {
    w.beginObject();
    w.field("component", icp.component);
    w.field("templateId", icp.template_id);
    w.field("score", icp.score);
    w.field("selected", icp.selected);
    w.field("success", icp.success);
    w.field("bidirectional", icp.bidirectional);
    w.key("transform");
    w.beginArray();
    for (double v : transformToJson(icp.transform)) w.value(v);
    w.endArray();
    w.endObject();
  }
  w.endArray();
  w.key("fits");
  w.beginArray();
  for (const auto& fit : result.quality.fits) {
    w.beginObject();
    w.field("component", fit.component);
    w.field("model", fit.model);
    w.field("pointCount", static_cast<std::int64_t>(fit.point_count));
    w.field("inlierCount", static_cast<std::int64_t>(fit.inlier_count));
    w.field("radiusMm", fit.radius_mm);
    w.fieldIfSet("radiusMode", fit.radius_mode);
    w.field("centerXMm", fit.center_x_mm);
    w.field("centerYMm", fit.center_y_mm);
    w.field("linePointXMm", fit.line_point_x_mm);
    w.field("linePointYMm", fit.line_point_y_mm);
    w.field("lineDirX", fit.line_dir_x);
    w.field("lineDirY", fit.line_dir_y);
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.str();
}

void emit(Outputs& outputs, const char* port, const ::gap::core::MeasurementValue& value) {
  lyflow::Measurement m;
  m.unit = "mm";
  if (value.status == ::gap::core::MeasurementStatus::kSuccess && value.value_mm) {
    m.value = *value.value_mm;
    m.ok = true;
  } else {
    m.value = std::numeric_limits<double>::quiet_NaN();
    m.ok = false;
    m.message = value.failure ? value.failure.message
                              : (value.status == ::gap::core::MeasurementStatus::kInactive
                                     ? "未启用"
                                     : "没有结果");
  }
  outputs.set(port, Data::measurement(std::move(m)));
}

Status compute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  // configPath 是 path 参数，空值已经在 validate 阶段挡掉了。
  const fs::path config = params.path("configPath");
  std::error_code ec;
  if (!fs::is_regular_file(config, ec)) {
    return Status::Error(Phase::Execute, "io", "配置文件不存在: " + config.string(), "configPath");
  }

  ::detection::domain::DetectionConfiguration configuration;
  try {
    YAML::Node node = YAML::LoadFile(config.string());
    // 模板目录：不给就按 gap_batch_runner 的规则从配置文件名推（<父目录>/<主名>）。
    fs::path templateDir = params.flag("deriveTemplateDir") ? fs::path{}
                                                            : params.path("templateDir");
    if (templateDir.empty()) templateDir = config.parent_path() / config.stem();
    if (node["common_settings"] && fs::is_directory(templateDir, ec)) {
      node["common_settings"]["save_template_path"] = templateDir.lexically_normal().string();
    }
    configuration = ::detection::parseDetectionConfiguration(node);
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "bad_param",
                         std::string("配置读不了: ") + e.what(), "configPath");
  }
  // 这个算子只测量，不许它往磁盘写模板或对齐结果。
  configuration.common.save_template = false;
  configuration.alignment.save_result = false;

  ::gap::core::MeasurementRequest request;
  request.sample_id = params.text("sampleId");
  request.primary_cloud = toPcl(*inputs.get("primary").asCloud());
  request.secondary_cloud = toPcl(*inputs.get("secondary").asCloud());
  request.configuration = configuration;
  request.options.input_frame = ::gap::core::PointCloudFrame::kSensorXZ;
  request.options.export_fit_points = params.flag("exportFitPoints");

  // 模型 ROI 路径：推理在 measure 之前，输入是这两片**原始 1280 槽**的云（H2/§7）。
  const fs::path model = params.flag("useModel") ? params.path("modelPath") : fs::path{};
  if (!model.empty()) {
    if (!fs::is_regular_file(model, ec)) {
      return Status::Error(Phase::Execute, "bad_param", "模型文件不存在: " + model.string(),
                           "modelPath");
    }
    std::string message;
    const ::gap::ml::OnnxRoiPredictor* predictor = predictorFor(model, &message);
    if (predictor == nullptr) {
      return Status::Error(Phase::Execute, "bad_param", message, "modelPath");
    }
    const std::string failure = applyModelRoi(*predictor, request);
    if (!failure.empty()) {
      return Status::Error(Phase::Execute, "model_roi_failed", failure);
    }
  }

  const ::gap::core::MeasurementEngine engine;
  const auto result = engine.measure(request);

  emit(outputs, "gap", result.gap);
  emit(outputs, "flush", result.flush);
  ctx.log(LogLevel::Info, qualityJson(result));
  if (result.failure) {
    ctx.log(LogLevel::Warn, std::string("measure 失败: ") + result.failure.message);
  }
  return Status::Ok();
}

}  // namespace

void registerMeasureReference(Registry& r) {
  OperatorDesc op;
  op.id = "gap.measure_reference";
  op.version = "1.0.0";
  op.label = "测量（参考）";
  op.category = "间隙/参考";
  op.keywords = {"reference", "blackbox", "对照", "基线"};
  op.doc =
      "整条主路径的黑盒实现：直接调 MeasurementEngine::measure，日志里带一份 quality JSON。"
      "历史对拍用：拆分算子的行为已有意偏离它，图上改任何上游算子都不影响它，只有 "
      "configPath 与两片输入云算数。输入必须是传感器 XZ 帧的原始剖面；save_template 与 "
      "save_result 被强制关掉，它不写模板也不存对齐结果。";
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "Master 剖面，传感器 XZ 帧。", true},
      Port{"secondary", "PointCloud", "Secondary", "Slave 剖面，传感器 XZ 帧。", true},
  };
  op.outputs = {
      Port{"gap", "Measurement", "Gap", "间隙，毫米。", true},
      Port{"flush", "Measurement", "Flush", "段差，毫米。", true},
  };

  Param configPath;
  configPath.name = "configPath";
  configPath.type = ParamType::Path;
  configPath.label = "Config";
  configPath.doc = "StandardGap.yml 的路径。";
  configPath.def = Value::text("");
  configPath.mode = "open";
  configPath.filters = {FileFilter{"YAML", {"yml", "yaml"}}};

  Param derive;
  derive.name = "deriveTemplateDir";
  derive.type = ParamType::Bool;
  derive.label = "Derive Template Dir";
  derive.doc = "按 gap_batch_runner 的规则从配置文件名推模板目录（<父目录>/<主名>）。";
  derive.def = Value::boolean(true);

  Param templateDir;
  templateDir.name = "templateDir";
  templateDir.type = ParamType::Path;
  templateDir.label = "Template Dir";
  templateDir.doc = "模板目录，写进 configuration.common.save_template_path。";
  templateDir.def = Value::text("");
  templateDir.mode = "dir";
  // Path 参数只要可见就是必填，所以自动推导时把它藏起来。
  templateDir.visibleWhen.param = "deriveTemplateDir";
  templateDir.visibleWhen.eq = Value::boolean(false);

  Param sampleId;
  sampleId.name = "sampleId";
  sampleId.type = ParamType::String;
  sampleId.label = "Sample Id";
  sampleId.doc = "只写进结果里，不影响计算。";
  sampleId.def = Value::text("");

  Param exportFitPoints;
  exportFitPoints.name = "exportFitPoints";
  exportFitPoints.type = ParamType::Bool;
  exportFitPoints.label = "Export Fit Points";
  exportFitPoints.doc = "对应 gap_batch_runner 的 --dump-ml-export。不影响 gap/flush 的值。";
  exportFitPoints.def = Value::boolean(false);

  // Path 参数只要可见就是必填（plan.cpp），所以模型路径靠一个开关藏起来。
  Param useModel;
  useModel.name = "useModel";
  useModel.type = ParamType::Bool;
  useModel.label = "Use Model ROI";
  useModel.doc = "走模型 ROI 路径：推理出四框写进 roi_override，再 measure（§7）。";
  useModel.def = Value::boolean(false);

  Param modelPath;
  modelPath.name = "modelPath";
  modelPath.type = ParamType::Path;
  modelPath.label = "Model";
  modelPath.doc = "ROI 分割模型（.onnx）。按「路径 + mtime」缓存实例（H2）。";
  modelPath.def = Value::text("");
  modelPath.mode = "open";
  modelPath.filters = {FileFilter{"ONNX", {"onnx"}}};
  modelPath.visibleWhen.param = "useModel";
  modelPath.visibleWhen.eq = Value::boolean(true);

  op.params = {configPath, derive, templateDir, sampleId, exportFitPoints, useModel, modelPath};
  op.capabilities = {false, false, true};
  op.compute = &compute;
  op.externalKey = &configKey;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
