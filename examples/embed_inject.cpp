// 运行时注入的验收工具（A1-8）：同一张图跑两遍 —— 一遍让源节点自己读盘，
// 一遍把它读出来的点云原样注入回去 —— 比对图级输出是否逐位相同。
//
// 用法：embed_inject --dll <lyflow_core.dll> [--node n_load] [--ports primary,secondary] <graph>...
// 退出码 0 = 每张图两遍的 outputs 逐字节相同。
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lyflow/client.hpp"

namespace {

std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string dirOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

std::vector<std::string> split(const std::string& text, char sep) {
  std::vector<std::string> out;
  std::string item;
  std::istringstream in(text);
  while (std::getline(in, item, sep)) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dll;
  std::string node = "n_load";
  std::string portList = "primary,secondary";
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dll" && i + 1 < argc) {
      dll = argv[++i];
    } else if (a == "--node" && i + 1 < argc) {
      node = argv[++i];
    } else if (a == "--ports" && i + 1 < argc) {
      portList = argv[++i];
    } else {
      files.push_back(a);
    }
  }
  const std::vector<std::string> ports = split(portList, ',');
  if (dll.empty() || files.empty() || ports.empty()) {
    std::cerr << "用法：embed_inject --dll <lyflow_core.dll> [--node n_load] "
                 "[--ports primary,secondary] <graph>...\n";
    return 2;
  }

  try {
    lyflow::Client client(dll);
    std::cout << "core " << client.version() << "，" << files.size() << " 张图，注入 " << node
              << " 的 " << portList << "\n";

    std::size_t bad = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
      const std::string graph = readAll(files[i]);
      if (graph.empty()) {
        std::cerr << "读不了 " << files[i] << "\n";
        return 2;
      }
      const std::string baseDir = dirOf(files[i]);

      lyflow::RunOptions fromFiles;
      fromFiles.runId = "file-" + std::to_string(i);
      fromFiles.baseDir = baseDir;
      fromFiles.noReuse = true;
      const lyflow::RunResult a = client.run(graph, fromFiles);

      lyflow::RunOptions injected;
      injected.runId = "inject-" + std::to_string(i);
      injected.baseDir = baseDir;
      injected.noReuse = true;
      bool missing = false;
      for (const std::string& port : ports) {
        // maxPoints=0 表示不抽样，拿到的就是节点产出的那一份
        const lyflow::CloudView view = client.cloud(a.runId, node, port, 0);
        if (!view.valid()) {
          std::cerr << files[i] << ": 取不到 " << node << "." << port << "\n";
          missing = true;
          break;
        }
        lyflow::InputCloud in;
        in.nodeId = node;
        in.port = port;
        const std::size_t n = view.pointCount();
        in.xyz.assign(view.xyz(), view.xyz() + n * 3);
        if (view.intensity()) in.intensity.assign(view.intensity(), view.intensity() + n);
        if (view.normals()) in.normals.assign(view.normals(), view.normals() + n * 3);
        injected.inputs.push_back(std::move(in));
      }
      if (missing) {
        bad += 1;
        continue;
      }

      const lyflow::RunResult b = client.run(graph, injected);

      if (a.status == b.status && a.outputs == b.outputs) {
        std::cout << "  " << files[i] << ": ok (" << a.status << ")\n";
        continue;
      }
      bad += 1;
      std::cerr << "不一致 " << files[i] << "\n";
      std::cerr << "  读盘: " << a.status << " " << a.outputs << "\n";
      std::cerr << "  注入: " << b.status << " " << b.outputs << "\n";
    }

    std::cout << (files.size() - bad) << " / " << files.size() << " 张图注入与读盘逐位一致\n";
    return bad == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "失败: " << e.what() << "\n";
    return 2;
  }
}
