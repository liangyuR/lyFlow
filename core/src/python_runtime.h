#pragma once
#include <string>

#include <nlohmann/json.hpp>

namespace lyflow {
class Registry;
namespace python_runtime {
nlohmann::json configure(Registry& registry, const std::string& config);
nlohmann::json status();
nlohmann::json refresh(Registry& registry);
} // namespace python_runtime
} // namespace lyflow
