#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace lyflow {
struct GraphContractUpgradeError {
  std::string nodeId;
  std::string message;
};

bool upgradeGraphContracts(const nlohmann::json& graph, const nlohmann::json& request,
                           nlohmann::json& upgraded, nlohmann::json& report,
                           GraphContractUpgradeError& error);
} // namespace lyflow
