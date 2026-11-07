#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lyflow/data.h"
#include "lyflow/json_writer.h"

namespace {

using Clock = std::chrono::steady_clock;

void writeDistribution(lyflow::JsonWriter& writer, const char* name, std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double p) {
    return values.at(static_cast<std::size_t>(std::ceil(p * values.size())) - 1);
  };
  writer.key(name);
  writer.beginObject();
  writer.field("p50Ms", percentile(0.5));
  writer.field("p95Ms", percentile(0.95));
  writer.field("minMs", values.front());
  writer.field("maxMs", values.back());
  writer.endObject();
}

} // namespace

int main(int argc, char** argv) {
  try {
    const std::size_t elements = argc > 1 ? std::stoull(argv[1]) : 4194304;
    const std::size_t iterations = argc > 2 ? std::stoull(argv[2]) : 101;
    if (elements == 0 || elements > 67108864 || iterations == 0 || iterations > 10000)
      throw std::invalid_argument("elements: 1..67108864; iterations: 1..10000");

    auto tensor = std::make_shared<lyflow::Tensor>();
    tensor->shape = {static_cast<std::int64_t>(elements)};
    tensor->data.resize(elements);
    for (std::size_t i = 0; i < elements; ++i)
      tensor->data[i] = static_cast<float>(i % 1000) * 0.125f;
    const std::shared_ptr<const lyflow::Tensor> immutable = tensor;
    tensor.reset();

    const lyflow::Data warm = lyflow::Data::tensor(immutable);
    const std::string expected = warm.valueJson();
    std::vector<double> coldTimes, warmTimes;
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      const lyflow::Data cold = lyflow::Data::tensor(immutable);
      auto begin = Clock::now();
      const std::string coldJson = cold.valueJson();
      auto end = Clock::now();
      coldTimes.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
      begin = Clock::now();
      const std::string warmJson = warm.valueJson();
      end = Clock::now();
      warmTimes.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
      if (coldJson != expected || warmJson != expected)
        throw std::runtime_error("metadata changed for identical tensor contents");
      checksum += coldJson.size() + warmJson.size();
    }

    lyflow::JsonWriter writer;
    writer.beginObject();
    writer.field("scope", std::string("tensor_metadata_microbenchmark"));
    writer.field("elements", static_cast<std::int64_t>(elements));
    writer.field("tensorBytes", static_cast<std::int64_t>(elements * sizeof(float)));
    writer.field("iterations", static_cast<std::int64_t>(iterations));
    writer.field("hardwareThreads", static_cast<std::int64_t>(std::thread::hardware_concurrency()));
    writer.field("checksum", static_cast<std::int64_t>(checksum));
    writeDistribution(writer, "cold", coldTimes);
    writeDistribution(writer, "warm", warmTimes);
    writer.endObject();
    std::cout << writer.str() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
