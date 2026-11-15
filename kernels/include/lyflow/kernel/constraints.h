#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lyflow::kernel::constraints {
inline constexpr const char* kRecordType = "cloud.Constraints3D.v1";
inline constexpr std::size_t kMaxConstraints = 4096;
inline constexpr std::size_t kMaxItemsTextBytes = 65536;

struct Constraint {
  std::string id;
  std::string type;
  std::array<double, 3> source{};
  std::array<double, 3> target{};
  std::array<double, 3> direction{};
  double weight = 1;
  double offset = 0;
  int datum = -1;
};

struct Error {
  std::string code;
  std::string field;
  std::string message;
};

bool parse3D(const nlohmann::json& data, const std::string& sourceFrame,
             const std::string& targetFrame, const std::string& mode, std::vector<Constraint>& out,
             Error& error);
bool construct3D(const std::string& itemsText, const std::string& sourceFrame,
                 const std::string& targetFrame, const std::string& mode, nlohmann::json& out,
                 Error& error);
} // namespace lyflow::kernel::constraints
