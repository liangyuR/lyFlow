#include "lyflow/client.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
Json output(const lyflow::Client& client, const std::string& run, const char* node,
            const char* port) {
  for (const auto& item : Json::parse(client.outputInfo(run, node)))
    if (item["port"] == port)
      return item["value"];
  throw std::runtime_error("Expected output is absent");
}
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: embed_profile <lyflow_core.dll>\n";
    return 2;
  }
  try {
    lyflow::Client client(argv[1]);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    lyflow::InputProfile raw;
    raw.nodeId = "source";
    raw.port = "profile";
    raw.xyz = {0, 0, 0, 1, 0, 0, nan, 0, nan, 2, 0, 0, 4, 0, 2, 5, 0, 2, 6, 0, 2, 77, 0, 88};
    raw.validity = {1, 1, 0, 1, 1, 1, 1, 0};
    raw.sourceIndices = {0, 2, 4, 6, 8, 10, 12, 14};
    raw.intensity = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int slot = 0; slot < 8; ++slot) {
      raw.normals.insert(raw.normals.end(), {0, 1, 0});
      raw.rgb.insert(raw.rgb.end(), {static_cast<unsigned char>(slot), 20, 30});
    }
    raw.descriptorJson =
        R"({"schemaVersion":1,"frame":"fixture-sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"synthetic-native-host","sourceCount":16})";
    auto invalid = raw;
    invalid.validity[0] = 2;
    bool rejected = false;
    try {
      client.retainProfileInput(invalid);
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Invalid validity must be rejected before run");
    auto input = raw;
    input.retained = client.retainProfileInput(input);
    input.xyz.clear();
    input.validity.clear();
    input.sourceIndices.clear();
    input.intensity.clear();
    input.normals.clear();
    input.rgb.clear();
    const std::string graph =
        R"GRAPH({"schemaVersion":1,"id":"profile-sdk","nodes":[{"id":"source","op":"profile.input","params":{}},{"id":"clean","op":"profile.clean","params":{"mode":"preserve_slots","smoothPoints":1,"minIntensity":0,"minValidPoints":3}},{"id":"left","op":"profile.fit_line","params":{"invalidPolicy":"valid_only","rangeEnabled":true,"range":[0,0.0021]}},{"id":"right","op":"profile.fit_line","params":{"invalidPolicy":"valid_only","rangeEnabled":true,"range":[0.0039,0.0061]}},{"id":"measure","op":"metrology.profile_gap_flush","params":{"referenceX":0.003,"maxExtrapolation":0.01}},{"id":"export","op":"profile.to_cloud","params":{"unit":"m"}}],"edges":[{"id":"e0","from":{"node":"source","port":"profile"},"to":{"node":"clean","port":"profile"}},{"id":"e1","from":{"node":"clean","port":"clean"},"to":{"node":"left","port":"profile"}},{"id":"e2","from":{"node":"clean","port":"clean"},"to":{"node":"right","port":"profile"}},{"id":"e3","from":{"node":"left","port":"line"},"to":{"node":"measure","port":"left"}},{"id":"e4","from":{"node":"right","port":"line"},"to":{"node":"measure","port":"right"}},{"id":"e5","from":{"node":"left","port":"statistics"},"to":{"node":"measure","port":"leftStatistics"}},{"id":"e6","from":{"node":"right","port":"statistics"},"to":{"node":"measure","port":"rightStatistics"}},{"id":"e7","from":{"node":"clean","port":"clean"},"to":{"node":"export","port":"profile"}}]})GRAPH";
    lyflow::RunOptions options;
    options.runId = "profile-sdk";
    options.profileInputs = {input};
    const auto run = client.run(graph, options);
    if (!run.ok()) {
      for (const auto& event : run.events)
        std::cerr << event << '\n';
      return 1;
    }
    const auto clean = client.profile(run.runId, "clean", "clean");
    require(clean.valid() && clean.pointCount() == 8 && clean.flags() == 7,
            "Clean Profile lost slots or channels");
    require(std::memcmp(clean.xyz(), raw.xyz.data(), raw.xyz.size() * sizeof(float)) == 0,
            "Clean changed raw coordinate bits");
    require(std::memcmp(clean.validity(), raw.validity.data(), 8) == 0, "Clean changed validity");
    require(std::memcmp(clean.sourceIndices(), raw.sourceIndices.data(), 8 * sizeof(unsigned)) == 0,
            "Clean changed source indices");
    require(std::memcmp(clean.intensity(), raw.intensity.data(), 8 * sizeof(float)) == 0,
            "Clean changed intensity");
    require(std::memcmp(clean.normals(), raw.normals.data(), 24 * sizeof(float)) == 0,
            "Clean changed normals");
    require(std::memcmp(clean.rgb(), raw.rgb.data(), 24) == 0, "Clean changed RGB");
    const auto metric = client.profile(run.runId, "export", "profile");
    require(metric.valid() && metric.pointCount() == 8 && metric.flags() == 7,
            "Metric export lost Profile channels");
    require(Json::parse(metric.descriptorJson())["unit"] == "m" &&
                Json::parse(metric.descriptorJson())["sourceCount"] == 16,
            "Metric descriptor or sourceCount changed");
    require(metric.validity()[7] == 0 && metric.sourceIndices()[7] == 14 &&
                std::isfinite(metric.xyz()[21]) && std::isnan(metric.xyz()[6]),
            "Metric invalid slots or original mapping changed");
    const auto gap = output(client, run.runId, "measure", "gap"),
               flush = output(client, run.runId, "measure", "flush");
    for (const auto& value : {gap, flush})
      require(value["ok"] == true && value["unit"] == "m" && value["value"].is_number() &&
                  std::isfinite(value["value"].get<double>()),
              "Typed metrology must return a finite meter measurement");
    std::cout << Json({{"type", "lyflow.ProfileSdkAcceptance.v1"},
                       {"slots", 8},
                       {"validSlots", 6},
                       {"unit", "m"},
                       {"optionalChannelsPreserved", true},
                       {"sourceMappingPreserved", true},
                       {"badInputRejected", rejected},
                       {"gap", gap},
                       {"flush", flush}})
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
