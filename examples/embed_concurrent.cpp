// 并发 run 的验收工具（A1-2）：把若干张图先串行跑一遍，再用 N 条线程并发跑一遍，
// 比对每张图的图级输出，并检查每条事件的 runId 都只属于自己那次运行。
//
// 用法：embed_concurrent --dll <lyflow_core.dll> [--threads 8] <graph>...
// 退出码 0 = 结果一致且事件隔离；1 = 有不一致。
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lyflow/client.hpp"

namespace {

struct Case {
  std::string path;
  std::string json;
  std::string baseDir;
};

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

/// 事件里的 runId。事件是一行 JSON，runId 紧跟 schemaVersion，形态稳定，
/// 为一个验收工具引一个 JSON 库不划算。
std::string runIdOf(const std::string& event) {
  const std::size_t at = event.find("\"runId\":\"");
  if (at == std::string::npos) return {};
  const std::size_t begin = at + 9;
  const std::size_t end = event.find('"', begin);
  return end == std::string::npos ? std::string() : event.substr(begin, end - begin);
}

struct Outcome {
  std::string status;
  std::string outputs;
  bool eventsIsolated = true;
  std::size_t eventCount = 0;
};

Outcome runOne(const lyflow::Client& client, const Case& c, const std::string& runId) {
  lyflow::RunOptions options;
  options.runId = runId;
  options.baseDir = c.baseDir;
  // 并发那一轮不吃缓存，否则第二张图会直接命中第一张图的结果，压不到任何东西。
  options.noReuse = true;

  Outcome out;
  std::size_t foreign = 0;
  const lyflow::RunResult result =
      client.runAsync(c.json, options, [&](const char* eventJson) {
        out.eventCount += 1;
        if (runIdOf(eventJson) != runId) foreign += 1;
      });
  out.status = result.status;
  out.outputs = result.outputs;
  out.eventsIsolated = foreign == 0;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dll;
  int threads = 8;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dll" && i + 1 < argc) {
      dll = argv[++i];
    } else if (a == "--threads" && i + 1 < argc) {
      threads = std::atoi(argv[++i]);
    } else {
      files.push_back(a);
    }
  }
  if (dll.empty() || files.empty()) {
    std::cerr << "用法：embed_concurrent --dll <lyflow_core.dll> [--threads 8] <graph>...\n";
    return 2;
  }
  if (threads < 1) threads = 1;

  std::vector<Case> cases;
  for (const std::string& f : files) {
    Case c;
    c.path = f;
    c.json = readAll(f);
    c.baseDir = dirOf(f);
    if (c.json.empty()) {
      std::cerr << "读不了 " << f << "\n";
      return 2;
    }
    cases.push_back(std::move(c));
  }

  try {
    lyflow::Client client(dll);
    std::cout << "core " << client.version() << "，" << cases.size() << " 张图，"
              << threads << " 条并发线程\n";

    std::vector<Outcome> serial(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
      serial[i] = runOne(client, cases[i], "serial-" + std::to_string(i));
    }

    std::vector<Outcome> parallel(cases.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&] {
        for (;;) {
          const std::size_t i = next.fetch_add(1);
          if (i >= cases.size()) return;
          parallel[i] = runOne(client, cases[i], "par-" + std::to_string(i));
        }
      });
    }
    for (auto& th : pool) th.join();

    std::size_t bad = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
      const bool sameStatus = serial[i].status == parallel[i].status;
      const bool sameOutputs = serial[i].outputs == parallel[i].outputs;
      const bool isolated = parallel[i].eventsIsolated && serial[i].eventsIsolated;
      if (sameStatus && sameOutputs && isolated) continue;
      bad += 1;
      std::cerr << "不一致 " << cases[i].path << "\n";
      if (!sameStatus) {
        std::cerr << "  status: " << serial[i].status << " vs " << parallel[i].status << "\n";
      }
      if (!sameOutputs) {
        std::cerr << "  outputs 串行: " << serial[i].outputs << "\n";
        std::cerr << "  outputs 并发: " << parallel[i].outputs << "\n";
      }
      if (!isolated) std::cerr << "  收到了别的 run 的事件\n";
    }

    std::cout << (cases.size() - bad) << " / " << cases.size() << " 张图串并一致，事件按 runId 隔离\n";
    return bad == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "失败: " << e.what() << "\n";
    return 2;
  }
}
