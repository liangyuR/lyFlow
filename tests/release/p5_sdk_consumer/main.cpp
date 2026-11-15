#include "lyflow/client.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  try {
    lyflow::Client client(argv[1]);
    if (client.version() != LYFLOW_EXPECTED_VERSION)
      throw std::runtime_error("Installed runtime version differs from the exact SDK version");
    const auto path = std::filesystem::u8path(argv[2]);
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw std::runtime_error("Cannot open the saved workflow");
    const std::string graph((std::istreambuf_iterator<char>(input)), {});
    if (input.bad())
      throw std::runtime_error("Cannot read the saved workflow");
    lyflow::RunOptions options;
    options.runId = "p5-isolated-sdk-consumer";
    options.baseDir = path.parent_path().u8string();
    options.maxParallel = 2;
    options.noReuse = true;
    const auto result = client.run(graph, options);
    std::cout << result.outputs << '\n';
    if (!result.ok()) {
      std::cerr << result.summary;
      return 3;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what();
    return 1;
  }
}
