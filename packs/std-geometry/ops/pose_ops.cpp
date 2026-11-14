#include "pose_common.h"

#include "lyflow/calibration.h"

namespace lyflow::packs::std_geometry {
namespace {

using namespace pose_detail;

std::vector<Param> identityParams(bool is2D) {
  return {text("id", "实例标识", "object-1"),
          text("frame", "参考帧", is2D ? "image" : "world"),
          text("name", "名称"),
          text("classId", "类别标识"),
          text("classLabel", "类别名称"),
          number("confidence", "置信度", 1),
          flag("pickable", "已确认可抓取", false)};
}

void metadata(Json& item, const ParamView& p) {
  for (const char* name : {"name", "classId", "classLabel"})
    if (!p.text(name).empty())
      item[name] = p.text(name);
  item["confidence"] = p.number("confidence");
  item["pickable"] = p.flag("pickable");
}

Status construct2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const auto& position = p.raw().at("position").vecValue();
  const auto& size = p.raw().at("size").vecValue();
  if (position.size() != 2 || size.size() != 2)
    return badParam("position/size 必须有两个分量", "position");
  const auto& unit = p.choice("unit");
  double x = 0, y = 0, angle = 0;
  for (const auto& pair :
       {std::pair<const char*, double*>{"x", &x}, {"y", &y}, {"angle", &angle}}) {
    const double fallback = pair.first == std::string("x")   ? position[0]
                            : pair.first == std::string("y") ? position[1]
                                                             : p.number("angle");
    Status status = scalar(in, pair.first, fallback,
                           pair.first == std::string("angle") ? "rad" : unit, *pair.second);
    if (!status.ok)
      return status;
  }
  Record result =
      poses::make2D(p.text("id"), p.text("frame"), unit, {x, y}, angle, {size[0], size[1]});
  metadata(result.data, p);
  return put(out, "pose", std::move(result));
}

Status construct3D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const auto& position = p.raw().at("position").vecValue();
  const auto& size = p.raw().at("size").vecValue();
  const auto& rotation = p.raw().at("quaternion").vecValue();
  if (position.size() != 3 || size.size() != 3 || rotation.size() != 4)
    return badParam("position/size/quaternion 分量数错误", "position");
  V3 xyz{};
  int i = 0;
  for (const char* name : {"x", "y", "z"}) {
    const auto status = scalar(in, name, position[i], "m", xyz[i]);
    if (!status.ok)
      return status;
    ++i;
  }
  poses::Quaternion q{rotation[0], rotation[1], rotation[2], rotation[3]};
  if (p.choice("rotationMode") == "euler_xyz") {
    V3 angles{};
    i = 0;
    for (const char* name : {"rx", "ry", "rz"}) {
      const auto status = scalar(in, name, p.number(name), "rad", angles[i]);
      if (!status.ok)
        return status;
      ++i;
    }
    const poses::Quaternion x{std::sin(angles[0] / 2), 0, 0, std::cos(angles[0] / 2)};
    const poses::Quaternion y{0, std::sin(angles[1] / 2), 0, std::cos(angles[1] / 2)};
    const poses::Quaternion z{0, 0, std::sin(angles[2] / 2), std::cos(angles[2] / 2)};
    q = poses::multiply(poses::multiply(x, y), z);
  } else {
    for (const char* name : {"rx", "ry", "rz"})
      if (in.has(name))
        return bad("四元数模式不消费欧拉角端口", name);
  }
  if (!poses::normalizeQuaternion(q))
    return badParam("四元数必须有限且非零", "quaternion");
  Record result = poses::make3D(p.text("id"), p.text("frame"), xyz, q, {size[0], size[1], size[2]});
  metadata(result.data, p);
  return put(out, "pose", std::move(result));
}

Status concat(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  Record a, b;
  Status s = read(in.get("a"), a);
  if (!s.ok)
    return s;
  s = read(in.get("b"), b);
  if (!s.ok)
    return s;
  if (a.type != b.type || a.data["frame"] != b.data["frame"] || a.data["unit"] != b.data["unit"])
    return bad("拼接要求相同维度、参考帧和单位");
  if (a.data["items"].size() + b.data["items"].size() > values::kMaxItems)
    return bad("拼接超过 4096 项");
  for (const auto& item : b.data["items"])
    a.data["items"].push_back(item);
  return put(out, "poses", std::move(a));
}

Status get(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  Record list;
  const Status s = read(in.get("poses"), list);
  if (!s.ok)
    return s;
  const auto index = p.integer("index");
  if (index < 0 || static_cast<std::size_t>(index) >= list.data["items"].size())
    return Status::Error(Phase::Execute, "index_out_of_range", "位姿索引越界", "index");
  Record item{two(list) ? poses::kPose2DType : poses::kPose3DType,
              list.data["items"][static_cast<std::size_t>(index)]};
  return put(out, "pose", std::move(item));
}

Status filter(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record list;
  Status s = read(in.get("poses"), list);
  if (!s.ok)
    return s;
  const auto& mode = p.choice("mode");
  const double minimum = p.number("minimum"), maximum = p.number("maximum");
  const bool range = mode == "confidence" || mode == "area" || mode == "attribute";
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || (range && minimum > maximum))
    return badParam("minimum/maximum 必须有限且有序", "minimum");
  if (mode == "distance" && minimum < 0)
    return badParam("最小间距必须非负", "minimum");
  const auto& center = p.raw().at("center").vecValue();
  const auto& tolerance = p.raw().at("tolerance").vecValue();
  if (center.size() != 3 || tolerance.size() != 3)
    return badParam("位置筛选需要三分量 center/tolerance", "center");
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(center[i]) || !std::isfinite(tolerance[i]) || tolerance[i] < 0)
      return badParam("center 必须有限，tolerance 必须有限非负", "tolerance");
  Json::json_pointer pointer;
  try {
    pointer = Json::json_pointer(p.text("path"));
  } catch (const std::exception&) {
    return badParam("path 不是合法 JSON Pointer", "path");
  }
  Json matched = Json::array(), rejected = Json::array();
  std::vector<V3> acceptedPositions;
  for (const auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    bool keep = false;
    if (mode == "name" || mode == "classId")
      keep = item.contains(mode) && item[mode] == p.text("text");
    else if (mode == "pickable")
      keep = item.contains("pickable") && item["pickable"] == p.flag("pickable");
    else if (mode == "confidence" || mode == "area" || mode == "attribute") {
      const Json* value = nullptr;
      Json area;
      if (mode == "confidence" && item.contains("confidence"))
        value = &item["confidence"];
      else if (mode == "area") {
        const double result = item["size"][0].get<double>() * item["size"][1].get<double>();
        if (!std::isfinite(result))
          return bad("面积超出有限范围");
        area = result;
        value = &area;
      } else if (mode == "attribute" && item.contains(pointer))
        value = &item.at(pointer);
      keep = value && values::safeNumber(*value) && value->get<double>() >= minimum &&
             value->get<double>() <= maximum;
    } else {
      V3 position{};
      for (std::size_t i = 0; i < item["position"].size(); ++i)
        position[i] = item["position"][i].get<double>();
      keep = true;
      if (mode == "position") {
        for (std::size_t i = 0; i < item["position"].size(); ++i)
          keep = keep && std::abs(position[i] - center[i]) <= tolerance[i];
      } else if (mode == "distance") {
        for (const V3& previous : acceptedPositions) {
          const V3 d = sub(position, previous);
          const double separation = std::hypot(d[0], d[1], d[2]);
          if (!std::isfinite(separation))
            return bad("实例间距超出有限范围");
          if (separation < minimum) {
            keep = false;
            break;
          }
        }
        if (keep)
          acceptedPositions.push_back(position);
      } else
        return badParam("未知筛选模式", "mode");
    }
    (keep ? matched : rejected).push_back(item);
  }
  Record failed = list;
  failed.data["items"] = std::move(rejected);
  list.data["items"] = std::move(matched);
  s = put(out, "matched", std::move(list));
  return s.ok ? put(out, "rejected", std::move(failed)) : s;
}

Status update(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record list;
  Status s = read(in.get("poses"), list);
  if (!s.ok)
    return s;
  Json patch;
  std::string why;
  if (!parseText(p.text("patch"), patch, why) || !patch.is_object())
    return badParam(why.empty() ? "patch 必须为对象" : why, "patch");
  const std::set<std::string> fields{"name",     "classId", "classLabel", "confidence",
                                     "pickable", "size",    "attributes"};
  for (auto it = patch.begin(); it != patch.end(); ++it)
    if (!fields.count(it.key()))
      return badParam("不允许修改身份、帧或位姿字段：" + it.key(), "patch");
  Record probe =
      two(list) ? poses::make2D("probe", list.data["frame"], list.data["unit"], {0, 0}, 0, {1, 1})
                : poses::make3D("probe", list.data["frame"], {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
  for (auto it = patch.begin(); it != patch.end(); ++it)
    probe.data[it.key()] = it.value();
  if (!poses::validate(probe, why))
    return badParam("属性补丁非法：" + why, "patch");
  for (auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    for (auto it = patch.begin(); it != patch.end(); ++it)
      item[it.key()] = it.value();
  }
  return put(out, "poses", std::move(list));
}

Status transformPoses(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx,
                      bool is2D) {
  Record list;
  std::string why;
  if (!poses::readList(in.get("poses"), is2D ? 2 : 3, list, why))
    return bad(why);
  if (list.data["frame"] != p.text("sourceFrame") || p.text("targetFrame").empty() ||
      (is2D && list.data["unit"] != p.choice("unit")))
    return badParam("输入帧/单位必须与 sourceFrame/unit 一致，targetFrame 不能为空", "sourceFrame");
  M4 m{};
  poses::Quaternion q{};
  double scale = 1;
  const bool valid = is2D ? similarity2D(in.get("transform").asTransform(), m, scale, true, why)
                          : rigid(in.get("transform").asTransform(), m, q, why);
  if (!valid)
    return bad(why, "transform");
  for (auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    V3 position{};
    for (int i = 0; i < (is2D ? 2 : 3); ++i)
      position[i] = item["position"][i].get<double>();
    const V3 result = pose_detail::apply(m, position);
    if (!finite(result))
      return bad("变换后的坐标超出有限范围");
    if (is2D) {
      const double angle = item["angle"].get<double>();
      item["angle"] = poses::wrapAngle(poses::wrapAngle(angle) + std::atan2(m[4], m[0]));
      item["position"] = {result[0], result[1]};
      for (auto& dimension : item["size"])
        dimension = dimension.get<double>() * scale;
    } else {
      item["position"] = result;
      item["rotation"] = poses::multiply(q, item["rotation"].get<poses::Quaternion>());
    }
    item["frame"] = p.text("targetFrame");
  }
  list.data["frame"] = p.text("targetFrame");
  return put(out, "poses", std::move(list));
}

Status offset3D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record list;
  std::string why;
  if (!poses::read3DList(in.get("poses"), list, why))
    return bad(why);
  const auto& values = p.raw().at("offset").vecValue();
  if (values.size() != 3)
    return badParam("offset 必须有三个分量", "offset");
  V3 offset{};
  int i = 0;
  for (const char* name : {"x", "y", "z"}) {
    const auto s = scalar(in, name, values[i], "m", offset[i]);
    if (!s.ok)
      return s;
    ++i;
  }
  for (auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    V3 delta = offset;
    if (p.choice("reference") == "local") {
      const auto rotation = poses::rotationMatrix(item["rotation"].get<poses::Quaternion>());
      for (int row = 0; row < 3; ++row)
        delta[row] = sum3(rotation[row * 3] * offset[0], rotation[row * 3 + 1] * offset[1],
                          rotation[row * 3 + 2] * offset[2]);
    }
    for (int j = 0; j < 3; ++j)
      item["position"][j] = item["position"][j].get<double>() + delta[j];
  }
  return put(out, "poses", std::move(list));
}

Status flip2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record list;
  std::string why;
  if (!poses::read2DList(in.get("poses"), list, why))
    return bad(why);
  const auto count = p.integer("symmetryOrder");
  if (count < 1 || count > 360)
    return badParam("对称阶数必须位于 [1,360]", "symmetryOrder");
  const double target = p.number("targetAngle");
  if (!std::isfinite(target))
    return badParam("目标方向必须为有限弧度", "targetAngle");
  const double period = 2 * poses::kPi / count;
  for (auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    const double angle = poses::wrapAngle(item["angle"].get<double>());
    const double k = std::round(poses::wrapAngle(poses::wrapAngle(target) - angle) / period);
    item["angle"] = poses::wrapAngle(angle + k * period);
  }
  return put(out, "poses", std::move(list));
}

Status frameFromPoints(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record a, b, c;
  std::string why;
  if (!poses::read3DList(in.get("a"), a, why) || !poses::read3DList(in.get("b"), b, why))
    return bad(why);
  const bool third = in.has("c");
  if (third && !poses::read3DList(in.get("c"), c, why))
    return bad(why);
  if (a.data["frame"] != b.data["frame"] || a.data["items"].size() != b.data["items"].size() ||
      (third &&
       (a.data["frame"] != c.data["frame"] || a.data["items"].size() != c.data["items"].size())))
    return bad("定位点集合必须同帧且长度相同");
  const auto& upValues = p.raw().at("up").vecValue();
  const auto& size = p.raw().at("size").vecValue();
  if (upValues.size() != 3 || size.size() != 3)
    return badParam("up/size 必须有三个分量", "up");
  for (double value : size)
    if (!std::isfinite(value) || value <= 0)
      return badParam("输出尺寸必须有限且为正", "size");
  V3 up{upValues[0], upValues[1], upValues[2]};
  if (!third && !normalize(up))
    return badParam("up 必须有限非零", "up");
  const double minSin = p.number("minimumSinAngle");
  if (!std::isfinite(minSin) || minSin <= 0 || minSin >= 1)
    return badParam("minimumSinAngle 必须位于 (0,1)", "minimumSinAngle");
  Json result = Json::array();
  for (std::size_t i = 0; i < a.data["items"].size(); ++i) {
    if (ctx.cancelled())
      return Status::Ok();
    const V3 pa = a.data["items"][i]["position"].get<V3>();
    const V3 pb = b.data["items"][i]["position"].get<V3>();
    V3 x = sub(pb, pa);
    if (!normalize(x))
      return bad("定位点 a/b 不能重合或溢出");
    V3 helper = up;
    V3 pc{};
    if (third) {
      pc = c.data["items"][i]["position"].get<V3>();
      helper = sub(pc, pa);
      if (!normalize(helper))
        return bad("第三点与第一点重合或溢出");
    }
    V3 y = cross(helper, x);
    const double sinAngle = std::hypot(y[0], y[1], y[2]);
    if (sinAngle < minSin || !normalize(y))
      return bad("定位点共线或两点方向与 up 近乎平行");
    V3 z = cross(x, y);
    if (third) {
      z = cross(x, helper);
      if (!normalize(z))
        return bad("三点无法形成右手坐标系");
      y = cross(z, x);
    }
    V3 origin = pa;
    if (p.choice("origin") == "centroid")
      for (int j = 0; j < 3; ++j)
        origin[j] = third ? pa[j] / 3 + pb[j] / 3 + pc[j] / 3 : pa[j] / 2 + pb[j] / 2;
    const poses::Matrix3 rotation{x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
    Record item = poses::make3D(a.data["items"][i]["id"], a.data["frame"], origin,
                                poses::quaternion(rotation), {size[0], size[1], size[2]});
    result.push_back(std::move(item.data));
  }
  return put(out, "poses", poses::make3DList(std::move(result), a.data["frame"]));
}

Status decompose2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  Record list;
  std::string why;
  if (!poses::read2DList(in.get("poses"), list, why))
    return bad(why);
  const auto index = p.integer("index");
  if (index < 0 || static_cast<std::size_t>(index) >= list.data["items"].size())
    return Status::Error(Phase::Execute, "index_out_of_range", "位姿索引越界", "index");
  const auto& item = list.data["items"][static_cast<std::size_t>(index)];
  const auto unit = list.data["unit"].get<std::string>();
  for (const auto& field : {std::pair<const char*, double>{"x", item["position"][0]},
                            {"y", item["position"][1]},
                            {"width", item["size"][0]},
                            {"height", item["size"][1]}})
    out.set(field.first, Data::record(values::number(field.second, unit)));
  out.set("angle", Data::record(values::number(item["angle"], "rad")));
  return Status::Ok();
}

Status compare2D(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  Record a, b;
  std::string why;
  if (!poses::read2DList(in.get("a"), a, why) || !poses::read2DList(in.get("b"), b, why))
    return bad(why);
  if (a.data["items"].size() != 1 || b.data["items"].size() != 1 ||
      a.data["frame"] != b.data["frame"] || a.data["unit"] != b.data["unit"])
    return bad("比较需要同帧同单位的两个单实例");
  const auto& aa = a.data["items"][0];
  const auto& bb = b.data["items"][0];
  const double dx = bb["position"][0].get<double>() - aa["position"][0].get<double>();
  const double dy = bb["position"][1].get<double>() - aa["position"][1].get<double>();
  const double distance = std::hypot(dx, dy), period = p.number("symmetryPeriod");
  const double positionLimit = p.number("positionTolerance"),
               angleLimit = p.number("angleTolerance");
  if (!std::isfinite(period) || period <= 0 || period > 2 * poses::kPi ||
      !std::isfinite(positionLimit) || positionLimit < 0 || !std::isfinite(angleLimit) ||
      angleLimit < 0)
    return badParam("周期必须位于(0,2π]，容差必须有限非负", "symmetryPeriod");
  const double angle = std::remainder(poses::wrapAngle(bb["angle"].get<double>()) -
                                          poses::wrapAngle(aa["angle"].get<double>()),
                                      period);
  if (!std::isfinite(distance))
    return bad("位置差超出有限范围");
  const std::string unit = a.data["unit"];
  out.set("distance", Data::record(values::number(distance, unit)));
  out.set("angle", Data::record(values::number(angle, "rad")));
  out.set("matched", Data::record(values::boolean(distance <= positionLimit &&
                                                  std::abs(angle) <= angleLimit)));
  out.set("difference", Data::record(Record{"vision.PoseDifference2D.v1",
                                            {{"frame", a.data["frame"]},
                                             {"unit", unit},
                                             {"dx", dx},
                                             {"dy", dy},
                                             {"distance", distance},
                                             {"angle", angle},
                                             {"symmetryPeriod", period}}}));
  return Status::Ok();
}

Status toText(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const auto* record = in.get("poses").asRecord();
  std::string why;
  if (!record || !poses::validate(*record, why))
    return bad(why);
  const std::string serialized = Json{{"type", record->type}, {"data", record->data}}.dump();
  if (serialized.size() > values::kMaxStringBytes)
    return bad("序列化文本超过 std.Value 字符串 64 KiB 上限；请先按实例拆分");
  out.set("text", Data::record(values::string(serialized)));
  return Status::Ok();
}

Status fromText(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const Record* record = nullptr;
  std::string why;
  if (!values::readValue(in.get("text"), record, why) || record->data["kind"] != "string")
    return bad(why.empty() ? "需要字符串值" : why, "text");
  Json value;
  if (!parseText(record->data["value"].get<std::string>(), value, why) || !value.is_object() ||
      value.size() != 2 || !value.contains("type") || !value["type"].is_string() ||
      !value.contains("data"))
    return bad(why.empty() ? "文本需要且仅允许 type/data" : why, "text");
  return put(out, "poses", Record{value["type"].get<std::string>(), value["data"]});
}

Status toTransform(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  Record list;
  Status s = read(in.get("poses"), list);
  if (!s.ok)
    return s;
  const auto index = p.integer("index");
  if (index < 0 || static_cast<std::size_t>(index) >= list.data["items"].size())
    return Status::Error(Phase::Execute, "index_out_of_range", "位姿索引越界", "index");
  if (list.data["unit"] != "m")
    return bad("原生 Transform 使用米制，像素位姿须先显式标定");
  const auto& item = list.data["items"][static_cast<std::size_t>(index)];
  poses::Quaternion q{};
  if (two(list)) {
    const double angle = poses::wrapAngle(item["angle"].get<double>()) / 2;
    q = {0, 0, std::sin(angle), std::cos(angle)};
  } else
    q = item["rotation"].get<poses::Quaternion>();
  const auto rotation = poses::rotationMatrix(q);
  Transform result;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col)
      result.m[row * 4 + col] = static_cast<float>(rotation[row * 3 + col]);
    const double coordinate = row < (two(list) ? 2 : 3) ? item["position"][row].get<double>() : 0;
    if (!nativeFloat(coordinate))
      return bad("位置超出原生 Transform float32 范围");
    result.m[row * 4 + 3] = static_cast<float>(coordinate);
  }
  out.set("transform", Data::transform(result));
  return Status::Ok();
}

Status toWorldPlane(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  Record list;
  std::string why;
  if (!poses::read2DList(in.get("poses"), list, why))
    return bad(why);
  const Record* record = in.get("calib").asRecord();
  calibration::Plane plane;
  if (!record || !calibration::parsePlane(*record, plane, why))
    return bad(why.empty() ? "需要平面标定" : why, "calib");
  if (list.data["unit"] != "px" || list.data["frame"] != p.text("sourceFrame") ||
      p.text("targetFrame").empty())
    return badParam("像素位姿帧必须匹配 sourceFrame，targetFrame 不能为空", "sourceFrame");
  M4 m{};
  poses::Quaternion planeRotation{};
  if (!rigid(in.get("planeToWorld").asTransform(), m, planeRotation, why))
    return bad(why, "planeToWorld");
  const auto& size = p.raw().at("size3D").vecValue();
  if (size.size() != 3)
    return badParam("size3D 必须有三个米制分量", "size3D");
  for (double value : size)
    if (!std::isfinite(value) || value <= 0)
      return badParam("size3D 必须有限且为正", "size3D");
  Json result = Json::array();
  for (const auto& item : list.data["items"]) {
    if (ctx.cancelled())
      return Status::Ok();
    const double x = item["position"][0], y = item["position"][1];
    double X = 0, Y = 0;
    if (!plane.map(x, y, X, Y))
      return bad("位姿中心投影位于单应性无穷远处");
    const auto& h = plane.H;
    const double w = h[6] * x + h[7] * y + h[8];
    const double angle = poses::wrapAngle(item["angle"].get<double>());
    const double dx =
        ((h[0] - X * h[6]) * std::cos(angle) + (h[1] - X * h[7]) * std::sin(angle)) / w;
    const double dy =
        ((h[3] - Y * h[6]) * std::cos(angle) + (h[4] - Y * h[7]) * std::sin(angle)) / w;
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) == 0)
      return bad("位姿方向无法在已知平面上投影");
    const double yaw = std::atan2(dy, dx);
    const V3 position =
        pose_detail::apply(m, {X * plane.metersPerUnit, Y * plane.metersPerUnit, 0});
    const poses::Quaternion yawRotation{0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};
    Record projected =
        poses::make3D(item["id"], p.text("targetFrame"), position,
                      poses::multiply(planeRotation, yawRotation), {size[0], size[1], size[2]});
    for (const char* field :
         {"classId", "classLabel", "name", "confidence", "pickable", "attributes"})
      if (item.contains(field))
        projected.data[field] = item[field];
    result.push_back(std::move(projected.data));
  }
  return put(out, "poses", poses::make3DList(std::move(result), p.text("targetFrame")));
}

} // namespace

void registerPoseOps(Registry& r) {
  OperatorDesc op = descriptor("pose.construct_2d", "二维工件位姿构造",
                               "construct input 2D pose radians identity size");
  op.doc = "中心位置、弧度朝向、正尺寸；动态 x/y/angle 是明确单位的 std.Value，覆盖对应常量。";
  op.inputs = {scalarPort("x"), scalarPort("y"), scalarPort("angle")};
  op.outputs = {record("pose")};
  op.params = identityParams(true);
  op.params.push_back(choice("unit", "坐标单位", "px", {"px", "m"}));
  op.params.push_back(vec("position", "中心位置", {0, 0}));
  op.params.push_back(number("angle", "朝向", 0, "rad"));
  op.params.push_back(vec("size", "尺寸", {1, 1}));
  op.compute = &construct2D;
  r.addOperator(std::move(op));

  op = descriptor("pose.construct_3d", "三维工件位姿构造",
                  "construct input 3D pose quaternion intrinsic XYZ radians");
  op.doc =
      "米制中心和尺寸；xyzw 四元数归一化并规范符号；可选内禀XYZ弧度，R=RxRyRz。动态位置/角度显式m/rad。";
  for (const char* port : {"x", "y", "z", "rx", "ry", "rz"})
    op.inputs.push_back(scalarPort(port));
  op.outputs = {record("pose")};
  op.params = identityParams(false);
  op.params.push_back(vec("position", "米制中心", {0, 0, 0}));
  op.params.push_back(vec("size", "米制尺寸", {1, 1, 1}));
  op.params.push_back(
      choice("rotationMode", "旋转表示", "quaternion", {"quaternion", "euler_xyz"}));
  op.params.push_back(vec("quaternion", "四元数 xyzw", {0, 0, 0, 1}));
  for (const char* name : {"rx", "ry", "rz"})
    op.params.push_back(number(name, name, 0, "rad"));
  op.compute = &construct3D;
  r.addOperator(std::move(op));

  op =
      descriptor("pose.concat", "工件实例集合拼接", "concat merge pose collection stable identity");
  op.doc = "单体或列表统一拼接，保留输入顺序和身份；重复ID、混帧/单位/维度返回错误，不自动重命名。";
  op.inputs = {record("a"), record("b")};
  op.outputs = {record("poses")};
  op.compute = &concat;
  r.addOperator(std::move(op));

  op = descriptor("pose.get", "工件实例索引提取", "pose get index select instance");
  op.inputs = {record("poses")};
  op.outputs = {record("pose")};
  op.params = {integer("index", "零基索引")};
  op.compute = &get;
  r.addOperator(std::move(op));

  op = descriptor("pose.filter", "工件实例条件筛选",
                  "pose filter confidence class name area distance translation attribute");
  op.doc =
      "保持顺序，分别输出通过与未通过集合。缺失可选字段不匹配；distance按已接受实例贪心筛最小间距，顺序显式影响结果。阈值使用实例单位，面积为平方单位。";
  op.inputs = {record("poses")};
  op.outputs = {record("matched"), record("rejected")};
  op.params = {choice("mode", "筛选方式", "confidence",
                      {"confidence", "classId", "name", "pickable", "area", "position", "distance",
                       "attribute"}),
               number("minimum", "最小值", 0),
               number("maximum", "最大值", 1),
               text("text", "匹配文本"),
               flag("pickable", "可抓取状态", true),
               vec("center", "位置中心", {0, 0, 0}),
               vec("tolerance", "位置容差", {1, 1, 1}),
               text("path", "数值JSON Pointer", "/attributes/value")};
  op.compute = &filter;
  r.addOperator(std::move(op));

  op = descriptor("pose.update", "工件实例属性更新", "pose reset update attributes dimension name");
  op.inputs = {record("poses")};
  op.outputs = {record("poses")};
  op.params = {text("patch", "JSON属性补丁", "{}")};
  op.doc =
      "显式替换 name/classId/classLabel/confidence/pickable/size/attributes；不修改ID/帧/位姿；attributes整体替换，结果须通过完整契约校验。";
  op.compute = &update;
  r.addOperator(std::move(op));

  for (bool is2D : {true, false}) {
    op = descriptor(is2D ? "pose.transform_2d" : "pose.transform_3d",
                    is2D ? "二维工件位姿变换" : "三维工件位姿变换",
                    "pose transform source target frame rigid similarity");
    op.inputs = {record("poses"),
                 Port{"transform", "Transform", "Transform", "source→target，左乘。", true}};
    op.outputs = {record("poses")};
    op.params = {text("sourceFrame", "源参考帧", is2D ? "image" : "world"),
                 text("targetFrame", "目标参考帧", "target")};
    if (is2D)
      op.params.push_back(choice("unit", "矩阵平移单位", "px", {"px", "m"}));
    op.doc =
        "明确源帧到目标帧左乘；三维只接受右手刚体，二维接受无镜像相似变换并缩放尺寸；保留实例身份与属性。";
    op.compute = [is2D](const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
      return transformPoses(in, p, out, ctx, is2D);
    };
    r.addOperator(std::move(op));
  }

  op = descriptor("pose.offset_3d", "三维工件位置偏移", "3D pose local world offset translation");
  op.inputs = {record("poses"), scalarPort("x"), scalarPort("y"), scalarPort("z")};
  op.outputs = {record("poses")};
  op.params = {vec("offset", "米制偏移", {0, 0, 0}),
               choice("reference", "偏移参考系", "local", {"local", "world"})};
  op.doc =
      "局部偏移先由实例旋转到世界参考帧；世界偏移直接相加。旋转、尺寸、帧和身份不变。动态值显式m。";
  op.compute = &offset3D;
  r.addOperator(std::move(op));

  op = descriptor("pose.flip_axis_2d", "二维工件对称轴规范化",
                  "2D flip pose axis symmetry direction");
  op.inputs = {record("poses")};
  op.outputs = {record("poses")};
  op.params = {integer("symmetryOrder", "显式旋转对称阶数", 2),
               number("targetAngle", "目标方向", 0, "rad")};
  op.doc =
      "仅在用户声明的n重旋转等价类中选最接近目标的方向；保持尺寸与ID，不推断矩形或物体具有该对称性。";
  op.compute = &flip2D;
  r.addOperator(std::move(op));

  op = descriptor("pose.frame_from_points", "定位点右手坐标系构造",
                  "frame from two three points orthonormal right handed");
  op.inputs = {record("a"), record("b"), record("c", false)};
  op.outputs = {record("poses")};
  op.params = {vec("up", "两点模式参考上方向", {0, 0, 1}), vec("size", "输出米制尺寸", {1, 1, 1}),
               choice("origin", "坐标原点", "centroid", {"first", "centroid"}),
               number("minimumSinAngle", "最小夹角正弦", 1e-6)};
  op.doc =
      "同长度同帧三维实例位置按序配对；连接c优先三点模式，X沿a→b、Z沿(a→b)×(a→c)。两点模式将up正交化；重合/共线拒绝。";
  op.compute = &frameFromPoints;
  r.addOperator(std::move(op));

  op = descriptor("pose.decompose_2d", "二维工件信息分解", "decompose 2D pose center angle size");
  op.inputs = {record("poses")};
  for (const char* name : {"x", "y", "angle", "width", "height"})
    op.outputs.push_back(scalarPort(name, true));
  op.params = {integer("index", "零基索引")};
  op.compute = &decompose2D;
  r.addOperator(std::move(op));

  op = descriptor("pose.compare_2d", "二维工件位姿比较",
                  "compare 2D pose distance angular symmetry tolerance");
  op.inputs = {record("a"), record("b")};
  op.outputs = {scalarPort("distance", true), scalarPort("angle", true),
                scalarPort("matched", true), record("difference")};
  op.params = {number("symmetryPeriod", "显式角度等价周期", 2 * poses::kPi, "rad"),
               number("positionTolerance", "位置容差", 0),
               number("angleTolerance", "角度容差", 0, "rad")};
  op.compute = &compare2D;
  r.addOperator(std::move(op));

  op = descriptor("pose.to_text", "工件位姿JSON序列化", "pose serialize string JSON roundtrip");
  op.inputs = {record("poses")};
  op.outputs = {scalarPort("text", true)};
  op.doc =
      "保存完整type/data、ID、帧、单位与属性，JSON最短往返精度；文本上限64KiB，超限须先分组，不能截断。";
  op.compute = &toText;
  r.addOperator(std::move(op));

  op = descriptor("pose.from_text", "工件位姿JSON解析",
                  "pose deserialize string JSON strict version schema");
  op.inputs = {scalarPort("text", true)};
  op.outputs = {record("poses")};
  op.compute = &fromText;
  r.addOperator(std::move(op));

  op = descriptor("pose.to_transform", "工件位姿米制矩阵提取",
                  "pose to native rigid transform matrix");
  op.inputs = {record("poses")};
  op.outputs = {
      Port{"transform", "Transform", "Transform", "所选实例局部→其参考帧的米制刚体矩阵。", true}};
  op.params = {integer("index", "零基索引")};
  op.compute = &toTransform;
  r.addOperator(std::move(op));

  op = descriptor("pose.to_world_plane", "二维工件已知平面位姿映射",
                  "2D object homography known plane world 3D pose projection");
  op.inputs = {record("poses"), record("calib"),
               Port{"planeToWorld", "Transform", "PlaneToWorld",
                    "已知平面坐标系→目标世界帧的米制刚体。", true}};
  op.outputs = {record("poses")};
  op.params = {text("sourceFrame", "图像参考帧", "image"),
               text("targetFrame", "目标参考帧", "world"),
               vec("size3D", "已知物理尺寸（米）", {0, 0, 0})};
  op.doc =
      "H将像素中心映到已知平面；方向由局部雅可比映射，再乘planeToWorld。3D尺寸必须显式给定，不由透视像素框猜物理尺寸；不估计任意深度或相机6DoF。";
  op.compute = &toWorldPlane;
  r.addOperator(std::move(op));
}

} // namespace lyflow::packs::std_geometry
