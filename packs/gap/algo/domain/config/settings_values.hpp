#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace domain::config {

enum class ApplicationLanguage {
  kEnglish = 0,
  kChinese = 1,
};

inline std::optional<ApplicationLanguage> ParseApplicationLanguage(std::string_view value) {
  if (value == "English") return ApplicationLanguage::kEnglish;
  if (value == "Chinese") return ApplicationLanguage::kChinese;
  return std::nullopt;
}

inline std::string_view ApplicationLanguageStorageValue(ApplicationLanguage language) {
  switch (language) {
    case ApplicationLanguage::kEnglish:
      return "English";
    case ApplicationLanguage::kChinese:
      return "Chinese";
  }
  return {};
}

enum class Theme {
  kWhite = 0,
  kBlack = 1,
};

inline std::optional<Theme> ParseTheme(std::string_view value) {
  if (value == "White") return Theme::kWhite;
  if (value == "Black") return Theme::kBlack;
  return std::nullopt;
}

inline std::string_view ThemeStorageValue(Theme theme) {
  switch (theme) {
    case Theme::kWhite:
      return "White";
    case Theme::kBlack:
      return "Black";
  }
  return {};
}

enum class SegmentationMode {
  kEach = 0,
  kTogether = 1,
  kRoi = 2,
};

inline std::optional<SegmentationMode> ParseSegmentationMode(std::string_view value) {
  if (value == "Each") return SegmentationMode::kEach;
  if (value == "Together") return SegmentationMode::kTogether;
  if (value == "ROI") return SegmentationMode::kRoi;
  return std::nullopt;
}

inline std::string_view SegmentationModeStorageValue(SegmentationMode mode) {
  switch (mode) {
    case SegmentationMode::kEach:
      return "Each";
    case SegmentationMode::kTogether:
      return "Together";
    case SegmentationMode::kRoi:
      return "ROI";
  }
  return {};
}

enum class CameraSelection {
  kBoth = 0,
  kLeft = 1,
  kRight = 2,
};

inline std::optional<CameraSelection> ParseCameraSelection(std::string_view value) {
  if (value == "Both") return CameraSelection::kBoth;
  if (value == "Left") return CameraSelection::kLeft;
  if (value == "Right") return CameraSelection::kRight;
  return std::nullopt;
}

inline std::string_view CameraSelectionStorageValue(CameraSelection selection) {
  switch (selection) {
    case CameraSelection::kBoth:
      return "Both";
    case CameraSelection::kLeft:
      return "Left";
    case CameraSelection::kRight:
      return "Right";
  }
  return {};
}

enum class RetentionPeriod {
  kSevenDays = 7,
  kFifteenDays = 15,
  kThirtyDays = 30,
};

inline std::optional<RetentionPeriod> ParseRetentionPeriod(int days) {
  switch (days) {
    case 7:
      return RetentionPeriod::kSevenDays;
    case 15:
      return RetentionPeriod::kFifteenDays;
    case 30:
      return RetentionPeriod::kThirtyDays;
    default:
      return std::nullopt;
  }
}

inline int RetentionPeriodStorageValue(RetentionPeriod period) { return static_cast<int>(period); }

// Prefix prepended to the MeasPoint.Name column of the production result CSV. "{model}" expands
// to the measured car type, so one global template covers every model (TM_{model}_ -> TM_KUN10_L6).
// The separator belongs to the configured value; nothing is inserted automatically. Empty keeps the
// raw point name.
inline constexpr std::string_view kMeasPointModelPlaceholder = "{model}";
inline constexpr std::size_t kMeasPointPrefixMaxLength = 32;

inline std::string_view TrimAsciiWhitespace(std::string_view value) {
  const auto is_space = [](char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
  };
  while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
  return value;
}

// Returns the trimmed prefix template, or nullopt when it carries characters that would disturb a
// customer CSV parser. Only letters, digits, "_", "-" and the "{model}" placeholder are accepted.
inline std::optional<std::string> ParseMeasPointPrefix(std::string_view value) {
  const auto trimmed = TrimAsciiWhitespace(value);
  if (trimmed.size() > kMeasPointPrefixMaxLength) return std::nullopt;
  for (std::size_t index = 0; index < trimmed.size();) {
    if (trimmed.substr(index, kMeasPointModelPlaceholder.size()) == kMeasPointModelPlaceholder) {
      index += kMeasPointModelPlaceholder.size();
      continue;
    }
    const auto character = static_cast<unsigned char>(trimmed[index]);
    if (std::isalnum(character) == 0 && character != '_' && character != '-') return std::nullopt;
    ++index;
  }
  return std::string(trimmed);
}

// Expands every "{model}" placeholder of an already validated prefix template.
inline std::string ExpandMeasPointPrefix(std::string_view prefix, std::string_view model) {
  std::string expanded;
  expanded.reserve(prefix.size() + model.size());
  for (std::size_t index = 0; index < prefix.size();) {
    if (prefix.substr(index, kMeasPointModelPlaceholder.size()) == kMeasPointModelPlaceholder) {
      expanded.append(model);
      index += kMeasPointModelPlaceholder.size();
      continue;
    }
    expanded.push_back(prefix[index]);
    ++index;
  }
  return expanded;
}

}  // namespace domain::config
