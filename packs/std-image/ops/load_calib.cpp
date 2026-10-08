// image.load_calib「读标定」（glue-plan K6）：宿主把工位的 image.PlaneCalib 存成 JSON 文件，产线图用它读回来。
#include <fstream>
#include <iterator>
#include <system_error>

#include "lyflow_cv/plane_calib.h"
#include "ops.h"

namespace lyflow::std_image {
namespace {

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext&) {
  const std::filesystem::path file = params.path("path");
  std::ifstream in(file, std::ios::binary);
  if (!in) return Status::Error(Phase::Execute, "io_error", "打不开标定文件：" + file.u8string(), "path");
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "bad_param", "标定文件不是合法的 JSON：" + std::string(e.what()), "path");
  }
  // 整个 Record（{kind, type, data}）或者只有 data 都认
  if (doc.is_object() && doc.contains("data") &&
      (doc.value("kind", nlohmann::json()) != "Record" || doc.value("type", nlohmann::json()) != kPlaneCalibType)) {
    return Status::Error(Phase::Execute, "bad_param", "标定 Record 的 kind/type 不匹配", "path");
  }
  const nlohmann::json data = doc.contains("data") && doc["data"].is_object() ? doc["data"] : doc;
  PlaneCalib calib;
  std::string why;
  if (!parsePlaneCalib(data, &calib, &why)) {
    return Status::Error(Phase::Execute, "bad_param", "标定文件不是合格的 image.PlaneCalib：" + why, "path");
  }
  Record rec;
  rec.type = kPlaneCalibType;
  rec.data = data;
  outputs.set("calib", Data::record(std::move(rec)));
  return Status::Ok();
}

std::string externalKey(const ParamView& params) {
  std::error_code ec;
  const auto file = params.path("path");
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(mtime.time_since_epoch().count());
}

}  // namespace

void registerLoadCalib(Registry& r) {
  OperatorDesc op;
  op.id = "image.load_calib";
  op.version = "1.0.0";
  op.label = "读标定";
  op.category = "图像/标定";
  op.keywords = {"calib", "calibration", "load", "标定", "读取", "单应"};
  op.doc = "读一份 image.PlaneCalib 的 JSON 文件（整个 Record 或只有 data 都行），出 Record。"
           "标定属于相机工位：宿主标一次存成文件，产线图经这个节点读，接到量测积木的 calib 口。";
  op.outputs = {Port{"calib", "Record", "Calib", "image.PlaneCalib。", true}};
  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "标定文件（JSON）。相对路径相对于图文件所在目录；中文路径可以。";
  path.def = Value::text("");
  path.mode = "open";
  path.filters = {FileFilter{"JSON", {"json"}}, FileFilter{"All Files", {"*"}}};
  op.params = {path};
  op.params.front().tuningRole = "input";
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::std_image
