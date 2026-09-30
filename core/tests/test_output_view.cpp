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

TEST_CASE("lyflow_output_tensor 的尾部：count=0 取到末尾、超出剩余元素数时截断，offset 越界是空切片而不是错误") {
  SUBCASE("count=0 取到末尾，超出剩余元素数时截断") {
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

  SUBCASE("offset 越界是空切片而不是错误") {
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
}

TEST_CASE("lyflow_output_tensor / lyflow_output_indices 的返回码：没有结果与类型不对都是 1，out 为空是 2") {
  SUBCASE("tensor") {
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

  SUBCASE("indices") {
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

TEST_CASE("lyflow_output_cloud 带 rgb（v12）：flags 第 3 位，抽样后与 xyz 同一组下标；没有 rgb 时为 NULL") {
  exec::ResultStore& store = exec::ResultStore::instance();
  store.clear();
  PointCloud colored;
  for (int i = 0; i < 10; ++i) {
    colored.push(static_cast<float>(i), 0.0f, 0.0f);
    colored.rgb.insert(colored.rgb.end(), {static_cast<std::uint8_t>(i * 10), 7, 255});
  }
  store.put("rgb", "n", "colored", "rgb-colored", Data::cloud(std::move(colored)));
  PointCloud plain;
  plain.push(1.0f, 2.0f, 3.0f);
  store.put("rgb", "n", "plain", "rgb-plain", Data::cloud(std::move(plain)));

  lyflow_cloud_view view{};
  REQUIRE(lyflow_output_cloud("rgb", "n", "colored", 5, &view) == 0);
  CHECK((view.flags & LYFLOW_CLOUD_HAS_RGB) != 0u);
  REQUIRE(view.rgb != nullptr);
  REQUIRE(view.point_count == 5u);
  // 等步长 2：第 k 个点是原来的第 2k 个，xyz 与 rgb 取的是同一个
  for (std::uint32_t k = 0; k < view.point_count; ++k) {
    CHECK(view.xyz[k * 3] == doctest::Approx(static_cast<float>(2 * k)));
    CHECK(view.rgb[k * 3] == 2 * k * 10);
    CHECK(view.rgb[k * 3 + 1] == 7);
    CHECK(view.rgb[k * 3 + 2] == 255);
  }
  lyflow_cloud_view_free(&view);

  lyflow_cloud_view bare{};
  REQUIRE(lyflow_output_cloud("rgb", "n", "plain", 0, &bare) == 0);
  CHECK((bare.flags & LYFLOW_CLOUD_HAS_RGB) == 0u);
  CHECK(bare.rgb == nullptr);
  lyflow_cloud_view_free(&bare);
  store.clear();
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

// ------------------------------------------- 图像（ABI v15，docs/image-plan.md §5.1）

TEST_CASE("lyflow_output_image：level 0 零拷贝按行切片；level 1 块均值缩小；级别过大收到 1 像素那级；越界是空切片；返回码") {
  exec::ResultStore& store = exec::ResultStore::instance();
  store.clear();
  // 6x4 RGB u8，像素值 = (x + 10y, 100, 200)
  Image img = Image::allocate(6, 4, 3, PixelDepth::U8);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 6; ++x) {
      std::uint8_t* p = img.mutablePixels() + (y * 6 + x) * 3;
      p[0] = static_cast<std::uint8_t>(x + 10 * y);
      p[1] = 100;
      p[2] = 200;
    }
  }
  store.put("img", "n", "image", "img-image", Data::image(img));
  PointCloud cloud;
  cloud.push(0, 0, 0);
  store.put("img", "n", "cloud", "img-cloud", Data::cloud(std::move(cloud)));

  SUBCASE("level 0：第 1 行起取 2 行，指针直接指进结果仓") {
    lyflow_image_view v{};
    REQUIRE(lyflow_output_image("img", "n", "image", 0, 1, 2, &v) == 0);
    CHECK(v.width == 6u);
    CHECK(v.height == 4u);
    CHECK(v.channels == 3u);
    CHECK(v.depth == 1u);
    CHECK(v.level == 0u);
    CHECK(v.full_width == 6u);
    CHECK(v.full_height == 4u);
    CHECK(v.row_offset == 1u);
    CHECK(v.row_count == 2u);
    CHECK(v.row_bytes == 18u);
    Data stored;
    REQUIRE(store.get("img", "n", "image", stored));
    CHECK(v.pixels == stored.asImage()->pixels.get() + 18);
    CHECK(v.pixels[0] == 10);                  // (0,1) 的 R
    CHECK(v.pixels[18 + 5 * 3] == 25);         // (5,2) 的 R
    // 结果仓清空之后借来的像素还活着（handle 持有）
    store.clear();
    CHECK(v.pixels[18 + 5 * 3 + 2] == 200);
    lyflow_image_view_free(&v);
    CHECK(v.handle == nullptr);
    CHECK(v.pixels == nullptr);
  }

  SUBCASE("level 1：3x2，第一格 = {0,1,10,11} 的均值；row_count=0 取到底") {
    lyflow_image_view v{};
    REQUIRE(lyflow_output_image("img", "n", "image", 1, 0, 0, &v) == 0);
    CHECK(v.width == 3u);
    CHECK(v.height == 2u);
    CHECK(v.level == 1u);
    CHECK(v.full_width == 6u);
    CHECK(v.row_count == 2u);
    CHECK(v.row_bytes == 9u);
    CHECK(v.pixels[0] == 6);  // 5.5 四舍五入
    CHECK(v.pixels[1] == 100);
    lyflow_image_view_free(&v);
  }

  SUBCASE("级别过大：收到长边 1 像素的那一级（6 → 3 → 2 → 1，level 3）") {
    lyflow_image_view v{};
    REQUIRE(lyflow_output_image("img", "n", "image", 30, 0, 0, &v) == 0);
    CHECK(v.level == 3u);
    CHECK(v.width == 1u);
    CHECK(v.height == 1u);
    lyflow_image_view_free(&v);
  }

  SUBCASE("row_offset 越界：成功、row_count = 0、宽高照给") {
    lyflow_image_view v{};
    CHECK(lyflow_output_image("img", "n", "image", 0, 99, 1, &v) == 0);
    CHECK(v.row_count == 0u);
    CHECK(v.pixels == nullptr);
    CHECK(v.width == 6u);
    lyflow_image_view_free(&v);
  }

  SUBCASE("返回码：不是图像 / 没有结果是 1，out 为空是 2") {
    lyflow_image_view v{};
    CHECK(lyflow_output_image("img", "n", "cloud", 0, 0, 0, &v) == 1);
    CHECK(v.handle == nullptr);
    CHECK(lyflow_output_image("img", "n", "nope", 0, 0, 0, &v) == 1);
    CHECK(lyflow_output_image("no-run", "n", "image", 0, 0, 0, &v) == 1);
    CHECK(lyflow_output_image("img", "n", "image", 0, 0, 0, nullptr) == 2);
  }
  store.clear();
}
