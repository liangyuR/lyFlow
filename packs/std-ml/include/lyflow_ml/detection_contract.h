#pragma once

#include <cmath>

#include "lyflow/manifest.h"

namespace lyflow::detection {

inline constexpr const char* kYOLOMappingType = "vision.YOLOPreprocess.v1";
inline constexpr const char* kYOLOXMappingType = "vision.YOLOXPreprocess.v1";
inline constexpr const char* kYOLOXModel = "yolox-nano-0.1.1rc0";
inline constexpr int kYOLOXInputSide = 416;
using Json = nlohmann::json;

inline bool familyValid(const std::string& family) {
  return family == "yolov8" || family == "yolo11" || family == "yolo26";
}

inline int roundedSize(double value) {
  const double base = std::floor(value), part = value - base;
  return static_cast<int>(base + (part > 0.5 || (part == 0.5 && std::fmod(base, 2) != 0)));
}

inline bool classesValid(const Json& value) {
  if (!value.is_array() || value.empty() || value.size() > 512)
    return false;
  for (const auto& item : value)
    if (!item.is_string() || item.get_ref<const std::string&>().empty() ||
        item.get_ref<const std::string&>().size() > 256)
      return false;
  return true;
}

inline Port yoloxRecordPort(const char* name, const char* type, const char* doc) {
  return withContract(Port{name, "Record", name, doc, true}, {{"recordType", type}});
}

inline Port yoloMappingPort() {
  return withContract(Port{"mapping", "Record", "变换", "同张图预处理输出。", true},
                      {{"recordType", kYOLOMappingType}});
}

inline constexpr const char* kCocoClasses[] = {"person",        "bicycle",      "car",
                                               "motorcycle",    "airplane",     "bus",
                                               "train",         "truck",        "boat",
                                               "traffic light", "fire hydrant", "stop sign",
                                               "parking meter", "bench",        "bird",
                                               "cat",           "dog",          "horse",
                                               "sheep",         "cow",          "elephant",
                                               "bear",          "zebra",        "giraffe",
                                               "backpack",      "umbrella",     "handbag",
                                               "tie",           "suitcase",     "frisbee",
                                               "skis",          "snowboard",    "sports ball",
                                               "kite",          "baseball bat", "baseball glove",
                                               "skateboard",    "surfboard",    "tennis racket",
                                               "bottle",        "wine glass",   "cup",
                                               "fork",          "knife",        "spoon",
                                               "bowl",          "banana",       "apple",
                                               "sandwich",      "orange",       "broccoli",
                                               "carrot",        "hot dog",      "pizza",
                                               "donut",         "cake",         "chair",
                                               "couch",         "potted plant", "bed",
                                               "dining table",  "toilet",       "tv",
                                               "laptop",        "mouse",        "remote",
                                               "keyboard",      "cell phone",   "microwave",
                                               "oven",          "toaster",      "sink",
                                               "refrigerator",  "book",         "clock",
                                               "vase",          "scissors",     "teddy bear",
                                               "hair drier",    "toothbrush"};

}
