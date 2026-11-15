#include "lyflow/client.hpp"

#include <cmath>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: embed_profile <lyflow_core.dll>\n";
    return 2;
  }
  try {
    lyflow::Client client(argv[1]);
    lyflow::InputProfile input;
    input.nodeId = "source";
    input.port = "out";
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    input.xyz = {1000, 0, 2000, nan, nan, nan, 3000, 0, 4000};
    input.validity = {1, 0, 0};
    input.sourceIndices = {0, 2, 3};
    input.descriptorJson =
        R"({"schemaVersion":1,"frame":"sensor","unit":"mm","plane":"XZ","handedness":"right","sampleOrder":"acquisition","sourceId":"sample-1","sourceCount":4})";
    input.retained = client.retainProfileInput(input);
    input.xyz.clear();
    input.validity.clear();
    input.sourceIndices.clear();
    const std::string graph =
        R"({"schemaVersion":1,"id":"profile-sdk","nodes":[{"id":"source","op":"util.reroute","params":{}},{"id":"export","op":"profile.to_cloud","params":{"unit":"m"}}],"edges":[{"id":"e","from":{"node":"source","port":"out"},"to":{"node":"export","port":"profile"}}],"outputs":{"profile":{"node":"export","port":"profile"}}})";
    lyflow::RunOptions options;
    options.runId = "profile-sdk";
    options.profileInputs = {input};
    const auto run = client.run(graph, options);
    if (!run.ok()) {
      for (const auto& event : run.events)
        std::cerr << event << '\n';
      return 1;
    }
    const auto view = client.profile(run.runId, "export", "profile");
    if (!view.valid() || view.pointCount() != 3 || view.sourceIndices()[1] != 2 ||
        view.validity()[2] != 0 || view.xyz()[0] != 1 || !std::isnan(view.xyz()[3]) ||
        view.xyz()[6] != 3)
      return 1;
    std::cout << view.descriptorJson() << '\n';
    std::cout << "Profile SDK: slots=3, valid=1, unit=m, sourceIndices=0,2,3\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
