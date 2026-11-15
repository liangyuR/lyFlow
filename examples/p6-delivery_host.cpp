#include "lyflow/frames.hpp"
#include <iostream>

int main(int argc, char** argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("usage: p6-delivery-host <core-library> [archive.lyframe|--legacy]");
    lyflow::Frame retained;
    lyflow::FrameSession retainedSession;
    lyflow::CloudView heldView;
    {
      lyflow::Client client(argv[1]);
      if (argc > 2 && std::string(argv[2]) == "--legacy") {
        try {
          client.runtimeCapabilities();
        } catch (const lyflow::ClientError& error) {
          if (std::string(error.what()).find("lyflow_runtime_capabilities") != std::string::npos) {
            std::cout << "older runtime loaded; optional capability unavailable\n";
            return 0;
          }
          throw;
        }
        throw std::runtime_error("legacy fixture unexpectedly has frame capabilities");
      }
      std::cout << client.runtimeCapabilities() << '\n';
      if (argc > 2) {
        retained = client.loadFrame(argv[2]);
      } else {
        const std::string graph =
            R"({"schemaVersion":1,"id":"p6-embed","name":"P6","nodes":[{"id":"source","op":"gen.synthetic","params":{"pointCount":100000,"withIntensity":true}}],"edges":[],"outputs":{"cloud":{"node":"source","port":"cloud"}}})";
        lyflow::RunOptions options;
        options.runId = "p6-cpp-first";
        auto result = client.run(graph, options);
        if (!result.ok())
          throw std::runtime_error("native run failed");
        retained = client.captureFrame(
            result.runId,
            R"({"frameId":"p6-cpp-frame","timestampNs":"1234567890123","frame":"world","calibrationVersion":null})");
        options.runId = "p6-cpp-replay";
        auto replay = client.runWithFrame(
            graph, options, retained,
            R"([{"sourceNode":"source","sourcePort":"cloud","nodeId":"source","port":"cloud"}])");
        if (!replay.ok())
          throw std::runtime_error("frame replay failed");
      }
      auto session = client.createSession(R"({"id":"p6-cpp","capacity":2,"maxBytes":67108864})");
      const auto before = session.info();
      if (session.push(retained, false, [] { return true; }).find("cancelled") ==
              std::string::npos ||
          session.info() != before)
        throw std::runtime_error("cancelled session push changed the queue");
      std::cout << session.push(retained) << '\n';
      auto queued = session.front();
      if (!queued.valid())
        throw std::runtime_error("session front missing");
      session.reset();
      if (session.front().valid())
        throw std::runtime_error("session reset failed");
      if (queued.metadata().find("p6-cpp") == std::string::npos)
        throw std::runtime_error("retained session frame invalid");
      session.push(retained);
      retainedSession = session;
      heldView = retained.cloud("source", "cloud");
    }
    if (!retained.valid() || retained.metadata().empty())
      throw std::runtime_error("frame did not retain runtime after Client disposal");
    if (!retainedSession.front().valid())
      throw std::runtime_error("session did not retain runtime after Client disposal");
    retainedSession = lyflow::FrameSession();
    auto cloud = retained.cloud("source", "cloud");
    if (!cloud.valid() || cloud.pointCount() != 100000 || !cloud.intensity())
      throw std::runtime_error("native cloud/channels not preserved");
    retained = lyflow::Frame();
    cloud = lyflow::CloudView();
    if (!heldView.valid() || heldView.pointCount() != 100000 || !heldView.xyz())
      throw std::runtime_error("view did not retain frame/runtime");
    std::cout << "C++17 frame/session ownership and replay passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
