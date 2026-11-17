#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

#include "lyflow/manifest.h"
#include "lyflow/profile.h"

namespace lyflow::profile_contract {
inline bool sameSpace(const ProfileDescriptor& a, const ProfileDescriptor& b) {
  return a.frame == b.frame && a.plane == b.plane && a.handedness == b.handedness;
}

inline Record geometryContext(const ProfileDescriptor& space) {
  return {"profile.Geometry.v3",
          {{"schemaVersion", 3},
           {"frame", space.frame},
           {"plane", space.plane},
           {"handedness", space.handedness}}};
}

inline bool validGeometry(const Data& geometry, std::string& why) {
  const auto finite = [](const float* values, int count) {
    return std::all_of(values, values + count, [](float value) { return std::isfinite(value); });
  };
  bool valid = false;
  if (const auto* point = geometry.asPoint2D()) {
    valid = point->unit == Unit2D::Meter && finite(point->p, 2);
  } else if (const auto* line = geometry.asLine2D()) {
    valid = line->unit == Unit2D::Meter && finite(line->point, 2) && finite(line->dir, 2) &&
            std::hypot(static_cast<double>(line->dir[0]), line->dir[1]) > 0;
    if (line->hasSegment)
      valid = valid && finite(line->start, 2) && finite(line->end, 2) &&
              (line->start[0] != line->end[0] || line->start[1] != line->end[1]);
  } else if (const auto* circle = geometry.asCircle2D()) {
    valid = circle->unit == Unit2D::Meter && finite(circle->center, 2) &&
            std::isfinite(circle->radius) && circle->radius > 0;
  } else if (const auto* box = geometry.asBox2D()) {
    valid = box->unit == Unit2D::Meter && finite(box->min, 2) && finite(box->max, 2) &&
            box->min[0] <= box->max[0] && box->min[1] <= box->max[1];
  }
  why = valid ? "" : "需要坐标有限、非退化的米制Point2D、Line2D、Circle2D或Box2D";
  return valid;
}

inline bool geometrySpace(const Data& geometry, const Record* context, ProfileDescriptor& space,
                          std::string& why) {
  if (!validGeometry(geometry, why))
    return false;
  const auto text = [](const nlohmann::json& value, const char* key) {
    return value.contains(key) && value[key].is_string();
  };
  if (!context || context->type != "profile.Geometry.v3" || !context->data.is_object() ||
      context->data.size() != 4 || !context->data.contains("schemaVersion") ||
      !context->data["schemaVersion"].is_number_integer() || context->data["schemaVersion"] != 3 ||
      !text(context->data, "frame") || !text(context->data, "plane") ||
      !text(context->data, "handedness")) {
    why = "需要profile.Geometry.v3几何上下文";
    return false;
  }
  space.frame = context->data["frame"].get<std::string>();
  space.plane = context->data["plane"].get<std::string>();
  space.handedness = context->data["handedness"].get<std::string>();
  why.clear();
  return true;
}

inline bool geometryInSpace(const Data& geometry, const Record* context,
                            const ProfileDescriptor& expected, std::string& why) {
  ProfileDescriptor space;
  if (!geometrySpace(geometry, context, space, why))
    return false;
  if (!sameSpace(space, expected)) {
    why = "几何与参照不在相同坐标帧、分析平面和手性";
    return false;
  }
  return true;
}

inline TopologyEdit retireInputs(const std::vector<MigrationInput>& inputs,
                                 std::initializer_list<const char*> retired) {
  TopologyEdit edit;
  for (const auto& input : inputs)
    if (std::find(retired.begin(), retired.end(), input.port) != retired.end()) {
      edit.dropInputs.push_back(input.port);
      edit.notes.push_back("删除已退役输入端口 " + input.port + " 的连线");
    }
  return edit;
}

inline TopologyEdit keepInputs(const nlohmann::json&, const std::vector<MigrationInput>&) {
  return {};
}
} // namespace lyflow::profile_contract
