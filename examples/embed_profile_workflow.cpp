#include "lyflow/client.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
Json output(const lyflow::Client& client, const std::string& run, const char* node,
            const char* port) {
  for (const auto& item : Json::parse(client.outputInfo(run, node)))
    if (item["port"] == port)
      return item["value"];
  throw std::runtime_error("Expected workflow output is absent");
}
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr
        << "usage: embed_profile_workflow <lyflow_core.dll> <m4-native-profile.lyflow.json>\n";
    return 2;
  }
  try {
    const auto graph = std::filesystem::canonical(std::filesystem::u8path(argv[2]));
    std::ifstream input(graph, std::ios::binary);
    require(static_cast<bool>(input), "Workflow file cannot be opened");
    const std::string document{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    lyflow::Client client(argv[1]);
    lyflow::RunOptions options;
    options.runId = "public-profile-workflow-sdk";
    options.baseDir = graph.parent_path().u8string();
    options.noReuse = true;
    const auto run = client.run(document, options);
    if (!run.ok()) {
      for (const auto& event : run.events)
        std::cerr << event << '\n';
      return 1;
    }
    const auto source = client.profile(run.runId, "source", "profile");
    const auto shared = client.profile(run.runId, "fit", "profile");
    require(source.valid() && shared.valid() && source.pointCount() == 16 &&
                shared.cloudId() == source.cloudId() && shared.xyz() == source.xyz() &&
                shared.validity() == source.validity() &&
                shared.sourceIndices() == source.sourceIndices(),
            "Fitter must preserve the original sixteen-slot Profile carrier");
    std::size_t valid = 0;
    for (std::size_t slot = 0; slot < source.pointCount(); ++slot) {
      valid += source.validity()[slot];
      require(source.sourceIndices()[slot] == slot, "CSV physical source-index mapping changed");
    }
    require(valid == 14 && source.validity()[2] == 0 && source.validity()[12] == 0 &&
                std::isnan(source.xyz()[6]) && std::isnan(source.xyz()[36]),
            "Invalid source slots were revived or compacted");
    const auto descriptor = Json::parse(source.descriptorJson());
    require(descriptor["unit"] == "mm" && descriptor["plane"] == "XZ" &&
                descriptor["frame"] == "m4-fixture",
            "The raw Profile descriptor was changed");
    const auto inliers = client.indices(run.runId, "fit", "inliers");
    const auto weighted = client.indices(run.runId, "fit", "weightedInliers");
    require(inliers.count() > 0 && weighted.count() >= inliers.count() &&
                inliers.sourceCloudId() == source.cloudId() &&
                weighted.sourceCloudId() == source.cloudId(),
            "Unique/weighted inliers lost their original cloud binding");
    const auto indexMap = output(client, run.runId, "fit", "indexMap")["data"];
    require(indexMap["sourceCloudId"] == std::to_string(source.cloudId()) &&
                indexMap["indexCount"] == inliers.count() &&
                indexMap["weightedInlierCount"] == weighted.count() &&
                indexMap["sourceIndexMapping"] == "profile.sourceIndices" && indexMap.size() == 6,
            "Small index-map contract changed");
    const auto selected = output(client, run.runId, "selected", "selection")["data"];
    const auto selectedSlot = selected["slotIndex"].get<std::size_t>();
    require(selectedSlot < source.pointCount() && source.validity()[selectedSlot] != 0 &&
                selected["sourceIndex"] == source.sourceIndices()[selectedSlot],
            "Nearest sample must identify a valid original source slot");
    const auto flush = output(client, run.runId, "flush", "value");
    const auto dy = output(client, run.runId, "offset", "dy");
    const auto distance = output(client, run.runId, "offset", "distance");
    for (const auto& value : {flush, dy, distance})
      require(value["ok"] == true && value["unit"] == "m" && value["value"].is_number() &&
                  std::isfinite(value["value"].get<double>()),
              "The shared workflow must return finite meter measurements");
    std::cout << Json({{"type", "lyflow.PublicProfileWorkflowAcceptance.v1"},
                       {"version", client.version()},
                       {"slots", source.pointCount()},
                       {"validSlots", valid},
                       {"uniqueInliers", inliers.count()},
                       {"weightedInliers", weighted.count()},
                       {"selectedSlot", selectedSlot},
                       {"inputProfileShared", true},
                       {"indexMapSmall", true},
                       {"flush", flush},
                       {"dy", dy},
                       {"distance", distance},
                       {"namedOutputs", Json::parse(run.outputs)}})
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
