#include <cmath>
#include <limits>
#include <stdexcept>

#include "dts_ops.h"
#include "lyflow/profile.h"

namespace lyflow::dts {
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::invalid_argument(message);
}

int integer(const nlohmann::json& data, const char* field) {
  const auto& value = data.at(field);
  require(value.is_number_integer(), "索引或点数必须为整数");
  const auto number = value.get<std::int64_t>();
  require(number >= 0 && number <= std::numeric_limits<int>::max(), "索引或点数超出范围");
  return static_cast<int>(number);
}
} // namespace

Status cancelledStatus() { return Status::Error(Phase::Execute, "cancelled", "DTS轮廓计算已取消"); }

nlohmann::json requireGraphMigration(const nlohmann::json&) {
  throw std::invalid_argument(
      "旧DTS节点需要dtsProfile.v1图级迁移（unit=mm、plane=XZ、sampleOrder=acquisition）：运行 lyflow migrate "
      "<图> --profile-source <dts.profile_in节点> --profile-frame <frame> --profile-source-id <sourceId> --write");
}

Status sceneError(const std::exception& error, const char* port) {
  return Status::Error(Phase::Execute, "bad_input", error.what(), {}, port);
}

Status loadProfile(const Data& source, ActiveProfile& output, ExecContext& ctx) {
  const auto* native = source.asProfile();
  if (native->descriptor.plane != "XZ" || native->descriptor.sampleOrder != "acquisition")
    return Status::Error(Phase::Execute, "bad_input", "DTS需要明确XZ平面和acquisition采样顺序", {},
                         "clean");
  if (native->pointCount() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return Status::Error(Phase::Execute, "bad_input", "轮廓槽数超过DTS处理范围", {}, "clean");
  ActiveProfile active;
  active.native = native;
  const double scale = native->descriptor.unit == "m" ? 1000.0 : 1.0;
  for (std::size_t i = 0; i < native->pointCount(); ++i) {
    if ((i & 4095) == 0 && ctx.cancelled())
      return cancelledStatus();
    if (!native->validity[i])
      continue;
    const double x = static_cast<double>(native->cloud->xyz[3 * i]) * scale;
    const double z = static_cast<double>(native->cloud->xyz[3 * i + 2]) * scale;
    if (!std::isfinite(x) || !std::isfinite(z) ||
        std::fabs(x) > std::numeric_limits<float>::max() ||
        std::fabs(z) > std::numeric_limits<float>::max())
      return Status::Error(Phase::Execute, "bad_input", "DTS毫米工作坐标超出范围", {}, "clean");
    active.samples.x.push_back(static_cast<float>(x));
    active.samples.z.push_back(static_cast<float>(z));
    active.slots.push_back(i);
  }
  output = std::move(active);
  return Status::Ok();
}

double number(const nlohmann::json& data, const char* field) {
  const auto& value = data.at(field);
  require(value.is_number(), "几何或长度字段必须为数值");
  const double result = value.get<double>();
  require(std::isfinite(result), "几何或长度字段必须有限");
  return result;
}

const nlohmann::json& checkedRecord(const Data& data, const char* type,
                                    const nlohmann::json& context, int schema) {
  const auto* record = data.asRecord();
  require(record && record->type == type, "场景Record类型或版本不匹配");
  const auto& value = record->data;
  require(value.is_object() && value.at("schemaVersion").is_number_integer() &&
              value.at("schemaVersion") == schema && value.at("workingUnit") == "mm",
          "场景Record需要明确版本和毫米工作单位");
  ProfileDescriptor descriptor;
  std::string why;
  require(profile_contract::parse(value.at("context"), descriptor, why), "场景Record上下文无效");
  require(descriptor.plane == "XZ" && descriptor.sampleOrder == "acquisition",
          "DTS场景上下文需要XZ采样轮廓");
  require(value.at("context") == context, "场景Record与轮廓来源、帧或内容不一致");
  return value;
}

nlohmann::json sceneRecord(const Data& source) {
  return {
      {"schemaVersion", 2}, {"workingUnit", "mm"}, {"context", profile_contract::context(source)}};
}

std::vector<Piece> checkedPieces(const Data& data, const Data& source,
                                 const ActiveProfile& active) {
  const auto& value = checkedRecord(data, "profile.Faces.v1", profile_contract::context(source), 1);
  require(value.at("indexSpace") == "active_samples" &&
              value.at("slotCount") == active.native->pointCount() &&
              value.at("activeCount") == active.samples.size(),
          "分片索引空间或输入槽数量不匹配");
  const auto pieces = piecesFromJson(value);
  int previous = -1;
  for (const auto& piece : pieces) {
    require(piece.i0 > previous && piece.i1 >= piece.i0 &&
                static_cast<std::size_t>(piece.i1) < active.samples.size(),
            "分片索引越界或重叠");
    previous = piece.i1;
  }
  return pieces;
}

std::vector<Piece> piecesFromJson(const nlohmann::json& data) {
  const auto& pieces = data.at("pieces");
  require(pieces.is_array(), "pieces必须为数组");
  std::vector<Piece> result;
  for (const auto& value : pieces) {
    Piece piece{integer(value, "i0"), integer(value, "i1")};
    require(piece.i1 >= piece.i0 && integer(value, "n") == piece.i1 - piece.i0 + 1,
            "分片范围与点数不一致");
    number(value, "x0");
    number(value, "x1");
    result.push_back(piece);
  }
  return result;
}

std::vector<Face> facesFromJson(const nlohmann::json& data) {
  const auto& faces = data.at("faces");
  require(faces.is_array(), "faces必须为数组");
  std::vector<Face> result;
  for (const auto& value : faces) {
    Face face;
    face.x0 = number(value, "x0");
    face.x1 = number(value, "x1");
    face.z0 = number(value, "z0");
    face.z1 = number(value, "z1");
    face.lenMm = number(value, "lenMm");
    face.angleDeg = number(value, "angleDeg");
    face.rmsMm = number(value, "rmsMm");
    face.n = integer(value, "n");
    face.line.px = number(value, "px");
    face.line.pz = number(value, "pz");
    face.line.dx = number(value, "dx");
    face.line.dz = number(value, "dz");
    face.line.rms = face.rmsMm;
    face.line.n = face.n;
    require(face.n >= 2 && face.x1 >= face.x0 && face.lenMm >= 0 && face.rmsMm >= 0 &&
                std::fabs(std::hypot(face.line.dx, face.line.dz) - 1) <= 1e-5,
            "面或直线拟合参数无效");
    result.push_back(face);
  }
  return result;
}

nlohmann::json faceToJson(const Face& face) {
  return {{"x0", face.x0},       {"x1", face.x1},       {"z0", face.z0},
          {"z1", face.z1},       {"lenMm", face.lenMm}, {"angleDeg", face.angleDeg},
          {"rmsMm", face.rmsMm}, {"n", face.n},         {"px", face.line.px},
          {"pz", face.line.pz},  {"dx", face.line.dx},  {"dz", face.line.dz}};
}

Line2D metricGeometry(const Line2D& legacy) {
  Line2D result = legacy;
  for (int i = 0; i < 2; ++i) {
    result.point[i] = static_cast<float>(static_cast<double>(legacy.point[i]) * .001);
    result.start[i] = static_cast<float>(static_cast<double>(legacy.start[i]) * .001);
    result.end[i] = static_cast<float>(static_cast<double>(legacy.end[i]) * .001);
  }
  return result;
}

Point2D metricGeometry(const Point2D& legacy) {
  Point2D result = legacy;
  for (int i = 0; i < 2; ++i)
    result.p[i] = static_cast<float>(static_cast<double>(legacy.p[i]) * .001);
  return result;
}

nlohmann::json lineMillimeters(const Line2D& line) {
  return {{"point", {line.point[0], line.point[1]}},
          {"dir", {line.dir[0], line.dir[1]}},
          {"hasSegment", line.hasSegment},
          {"start", {line.start[0], line.start[1]}},
          {"end", {line.end[0], line.end[1]}}};
}

Line2D lineMillimeters(const nlohmann::json& data) {
  Line2D line;
  line.hasSegment = data.at("hasSegment").get<bool>();
  for (const auto* key : {"point", "dir", "start", "end"})
    require(data.at(key).is_array() && data.at(key).size() == 2, "毫米基准线字段维度无效");
  for (int i = 0; i < 2; ++i) {
    line.point[i] = data.at("point").at(i).get<float>();
    line.dir[i] = data.at("dir").at(i).get<float>();
    line.start[i] = data.at("start").at(i).get<float>();
    line.end[i] = data.at("end").at(i).get<float>();
  }
  std::string why;
  require(profile_contract::validLine(Data::line2d(line), why), "毫米基准线坐标或方向无效");
  return line;
}

const nlohmann::json* recordData(const Data& data, const char* type) {
  const auto* record = data.asRecord();
  return record && (!type || record->type == type) ? &record->data : nullptr;
}
} // namespace lyflow::dts
