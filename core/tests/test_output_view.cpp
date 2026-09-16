#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "exec/result_store.h"
#include "lyflow/c_api.h"
#include "lyflow/data.h"

using namespace lyflow;

namespace {

void seedOutputs(const std::string& runId) {
  exec::ResultStore& store = exec::ResultStore::instance();
  store.clear();

  Tensor t;
  t.shape = {2, 3, 4};
  t.data.resize(24);
  for (std::size_t i = 0; i < t.data.size(); ++i) t.data[i] = static_cast<float>(i);
  store.put(runId, "n", "tensor", runId + "-tensor", Data::tensor(std::move(t)));

  Indices ix;
  ix.sourceCloudId = 4242;
  for (int i = 0; i < 10; ++i) ix.values.push_back(i * 3);
  store.put(runId, "n", "indices", runId + "-indices", Data::indices(std::move(ix)));

  PointCloud cloud;
  cloud.push(1.0f, 2.0f, 3.0f);
  store.put(runId, "n", "cloud", runId + "-cloud", Data::cloud(std::move(cloud)));
}

}  // namespace

TEST_CASE("lyflow_output_tensor 切出第二片，shape 仍是完整的 (2,3,4)") {
  const std::string run = "tensor-slice";
  seedOutputs(run);

  lyflow_tensor_view view{};
  REQUIRE(lyflow_output_tensor(run.c_str(), "n", "tensor", 6, 6, &view) == 0);
  CHECK(view.rank == 3u);
  CHECK(view.count == 6u);
  CHECK(view.offset == 6u);
  CHECK(view.total == 24u);

  REQUIRE(view.shape != nullptr);
  CHECK(view.shape[0] == 2);
  CHECK(view.shape[1] == 3);
  CHECK(view.shape[2] == 4);

  REQUIRE(view.data != nullptr);
  for (std::uint32_t i = 0; i < view.count; ++i) {
    CHECK(view.data[i] == doctest::Approx(static_cast<float>(6 + i)));
  }

  Data stored;
  REQUIRE(exec::ResultStore::instance().get(run, "n", "tensor", stored));
  CHECK(view.data == stored.asTensor()->data.data() + 6);
  CHECK(view.shape == stored.asTensor()->shape.data());

  lyflow_tensor_view_free(&view);
  CHECK(view.handle == nullptr);
  CHECK(view.data == nullptr);
  CHECK(view.count == 0u);
}

TEST_CASE("lyflow_output_tensor 的 count=0 取到末尾，超出剩余元素数时截断") {
  const std::string run = "tensor-tail";
  seedOutputs(run);

  lyflow_tensor_view whole{};
  REQUIRE(lyflow_output_tensor(run.c_str(), "n", "tensor", 0, 0, &whole) == 0);
  CHECK(whole.count == 24u);
  CHECK(whole.total == 24u);
  CHECK(whole.data[23] == doctest::Approx(23.0f));
  lyflow_tensor_view_free(&whole);

  lyflow_tensor_view tail{};
  REQUIRE(lyflow_output_tensor(run.c_str(), "n", "tensor", 20, 0, &tail) == 0);
  CHECK(tail.count == 4u);
  CHECK(tail.offset == 20u);
  CHECK(tail.rank == 3u);
  CHECK(tail.data[0] == doctest::Approx(20.0f));
  lyflow_tensor_view_free(&tail);

  lyflow_tensor_view clipped{};
  REQUIRE(lyflow_output_tensor(run.c_str(), "n", "tensor", 20, 999, &clipped) == 0);
  CHECK(clipped.count == 4u);
  CHECK(clipped.data[3] == doctest::Approx(23.0f));
  lyflow_tensor_view_free(&clipped);
}

TEST_CASE("lyflow_output_tensor 的 offset 越界是空切片而不是错误") {
  const std::string run = "tensor-past-end";
  seedOutputs(run);

  lyflow_tensor_view view{};
  CHECK(lyflow_output_tensor(run.c_str(), "n", "tensor", 1000, 4, &view) == 0);
  CHECK(view.count == 0u);
  CHECK(view.data == nullptr);
  CHECK(view.total == 24u);
  CHECK(view.rank == 3u);
  REQUIRE(view.shape != nullptr);
  CHECK(view.shape[2] == 4);
  lyflow_tensor_view_free(&view);

  lyflow_tensor_view edge{};
  CHECK(lyflow_output_tensor(run.c_str(), "n", "tensor", 24, 0, &edge) == 0);
  CHECK(edge.count == 0u);
  CHECK(edge.data == nullptr);
  lyflow_tensor_view_free(&edge);
}

TEST_CASE("lyflow_output_tensor 的返回码：没有结果与类型不对都是 1，out 为空是 2") {
  const std::string run = "tensor-codes";
  seedOutputs(run);

  lyflow_tensor_view view{};
  CHECK(lyflow_output_tensor(run.c_str(), "n", "cloud", 0, 0, &view) == 1);
  CHECK(view.handle == nullptr);
  CHECK(lyflow_output_tensor(run.c_str(), "n", "indices", 0, 0, &view) == 1);
  CHECK(lyflow_output_tensor(run.c_str(), "n", "nope", 0, 0, &view) == 1);
  CHECK(lyflow_output_tensor("no-such-run", "n", "tensor", 0, 0, &view) == 1);
  CHECK(lyflow_output_tensor(run.c_str(), "n", "tensor", 0, 0, nullptr) == 2);
}

TEST_CASE("lyflow_output_indices：sourceCloudId 原样带出，offset/count 分页") {
  const std::string run = "indices-page";
  seedOutputs(run);

  lyflow_indices_view head{};
  REQUIRE(lyflow_output_indices(run.c_str(), "n", "indices", 0, 4, &head) == 0);
  CHECK(head.count == 4u);
  CHECK(head.total == 10u);
  CHECK(head.source_cloud_id == 4242u);
  REQUIRE(head.values != nullptr);
  CHECK(head.values[0] == 0);
  CHECK(head.values[3] == 9);

  Data stored;
  REQUIRE(exec::ResultStore::instance().get(run, "n", "indices", stored));
  CHECK(head.values == stored.asIndices()->values.data());
  lyflow_indices_view_free(&head);
  CHECK(head.handle == nullptr);
  CHECK(head.values == nullptr);

  lyflow_indices_view tail{};
  REQUIRE(lyflow_output_indices(run.c_str(), "n", "indices", 8, 0, &tail) == 0);
  CHECK(tail.count == 2u);
  CHECK(tail.total == 10u);
  CHECK(tail.values[0] == 24);
  CHECK(tail.values[1] == 27);
  lyflow_indices_view_free(&tail);

  lyflow_indices_view past{};
  CHECK(lyflow_output_indices(run.c_str(), "n", "indices", 99, 4, &past) == 0);
  CHECK(past.count == 0u);
  CHECK(past.values == nullptr);
  CHECK(past.total == 10u);
  CHECK(past.source_cloud_id == 4242u);
  lyflow_indices_view_free(&past);
}

TEST_CASE("lyflow_output_indices 对非 Indices 端口返回 1") {
  const std::string run = "indices-codes";
  seedOutputs(run);

  lyflow_indices_view view{};
  CHECK(lyflow_output_indices(run.c_str(), "n", "tensor", 0, 0, &view) == 1);
  CHECK(view.handle == nullptr);
  CHECK(lyflow_output_indices(run.c_str(), "n", "cloud", 0, 0, &view) == 1);
  CHECK(lyflow_output_indices(run.c_str(), "n", "nope", 0, 0, &view) == 1);
  CHECK(lyflow_output_indices("no-such-run", "n", "indices", 0, 0, &view) == 1);
  CHECK(lyflow_output_indices(run.c_str(), "n", "indices", 0, 0, nullptr) == 2);
}

TEST_CASE("视图的 handle 让借来的数据活过结果仓的清空") {
  const std::string run = "view-keepalive";
  seedOutputs(run);

  lyflow_tensor_view view{};
  REQUIRE(lyflow_output_tensor(run.c_str(), "n", "tensor", 0, 0, &view) == 0);
  lyflow_indices_view ix{};
  REQUIRE(lyflow_output_indices(run.c_str(), "n", "indices", 0, 0, &ix) == 0);

  exec::ResultStore::instance().clear();

  CHECK(view.count == 24u);
  CHECK(view.data[23] == doctest::Approx(23.0f));
  CHECK(view.shape[0] == 2);
  CHECK(ix.count == 10u);
  CHECK(ix.values[9] == 27);
  CHECK(ix.source_cloud_id == 4242u);

  lyflow_tensor_view_free(&view);
  lyflow_indices_view_free(&ix);
}
