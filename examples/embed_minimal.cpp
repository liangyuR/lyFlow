// 嵌入 SDK 的最小宿主：加载 core DLL、跑一张合成图、读图级输出与点云。
// 用法：embed_minimal [<lyflow_core.dll 的路径>]，缺省找自己同目录的那一份。
#include <cstdlib>
#include <iostream>
#include <string>

#include "lyflow/client.hpp"

namespace {

const char* kGraph = R"({
  "schemaVersion": 1,
  "id": "01EMBEDMINIMAL0000000000000",
  "name": "embed_minimal",
  "nodes": [
    { "id": "src", "op": "gen.synthetic", "params": { "pointCount": 4096 } },
    { "id": "pipe", "op": "util.reroute" }
  ],
  "edges": [
    { "id": "e0", "from": { "node": "src", "port": "cloud" },
                  "to": { "node": "pipe", "port": "in" } }
  ],
  "outputs": { "cloud": { "node": "pipe", "port": "out" } }
})";

std::string defaultDll(const char* argv0) {
  std::string exe(argv0 ? argv0 : "");
  const std::size_t cut = exe.find_last_of("/\\");
  const std::string dir = cut == std::string::npos ? std::string(".") : exe.substr(0, cut);
#if defined(_WIN32)
  return dir + "\\lyflow_core.dll";
#else
  return dir + "/liblyflow_core.so";
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dll = argc > 1 ? std::string(argv[1]) : defaultDll(argv[0]);
  try {
    lyflow::Client client(dll);
    std::cout << "core " << client.version() << "，ABI v" << lyflow::kClientAbiVersion << "\n";

    const std::string problems = client.problems();
    if (!problems.empty()) {
      std::cerr << "算子描述自检有问题：\n" << problems << "\n";
      return 1;
    }

    const std::string diags = client.validate(kGraph);
    if (diags != "[]") {
      std::cerr << "校验失败：" << diags << "\n";
      return 1;
    }

    lyflow::RunOptions options;
    options.runId = "embed-minimal-1";
    options.maxParallel = 1;
    const lyflow::RunResult result = client.run(kGraph, options);

    std::cout << "run " << result.status << "，" << result.events.size() << " 条事件\n";
    std::cout << "outputs " << result.outputs << "\n";
    if (!result.ok()) {
      for (const std::string& d : result.diagnostics) std::cerr << d << "\n";
      return 1;
    }
    if (result.outputs.find("\"cloud\"") == std::string::npos) {
      std::cerr << "图输出里没有 cloud\n";
      return 1;
    }

    const lyflow::CloudView cloud = client.cloud(result.runId, "pipe", "out", 1000);
    if (!cloud.valid()) {
      std::cerr << "取不到 pipe.out 的点云\n";
      return 1;
    }
    std::cout << "cloud " << cloud.pointCount() << " / " << cloud.totalPoints() << " 点\n";
    if (cloud.totalPoints() != 4096) {
      std::cerr << "点数不对，期望 4096\n";
      return 1;
    }

    std::cout << "embed_minimal ok\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "失败: " << e.what() << "\n";
    return 1;
  }
}
