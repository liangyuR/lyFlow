// 生效参数视图（m6-plan §2 / H6）：每节点每参数的值与来源。
// 稀疏存储是对的，但「现在到底跑的是什么值」只能由合并默认值的那一层说 ——
// 这里钉住的就是「它说的与执行器用的是同一份」。
#include <doctest/doctest.h>

#include "exec/executor.h"
#include "helpers.h"
#include "test_ops.h"

namespace lyflow::test {
namespace {

Json paramsOf(const Json& doc) {
  return Json::parse(exec::effectiveParamsJson(doc.dump(), {}));
}

/// node → param → 那一行。找不到时 REQUIRE 失败，不静默返回空。
Json rowOf(const Json& view, const std::string& node, const std::string& param) {
  for (const Json& n : view["nodes"]) {
    if (n["node"] != node) continue;
    for (const Json& p : n["params"]) {
      if (p["param"] == param) return p;
    }
  }
  FAIL("没有 ", node, ".", param, "：", view.dump());
  return Json::object();
}

}  // namespace

TEST_CASE("params：图里写了的是 explicit，没写的是 default 且等于 manifest 默认值") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_src", "test.counted", Json{{"pointCount", 12}}},
                              N{"n_thin", "test.thin", Json::object()}},
                             {E{"n_src.cloud", "n_thin.cloud"}});
  const Json view = paramsOf(doc);

  const Json count = rowOf(view, "n_src", "pointCount");
  CHECK(count["source"] == "explicit");
  CHECK(count["value"] == 12);

  // 同一个节点上没写的那个参数
  const Json seed = rowOf(view, "n_src", "seed");
  CHECK(seed["source"] == "default");
  CHECK(seed["value"] == 0);

  // 向量参数与单位一起出来：Inspector 与 CLI 的表格都直接用这几格
  const Json leaf = rowOf(view, "n_thin", "leaf");
  CHECK(leaf["source"] == "default");
  CHECK(leaf["value"] == Json::array({0.02, 0.02, 0.02}));
  CHECK(leaf["unit"] == "m");
  CHECK(leaf["min"] == 0.0001);

  // op 跟着每个节点走：读 params 的人不必再去 join 一遍图
  bool sawOp = false;
  for (const Json& n : view["nodes"]) {
    if (n["node"] == "n_thin") {
      CHECK(n["op"] == "test.thin");
      sawOp = true;
    }
  }
  CHECK(sawOp);
}

TEST_CASE("params：生效值是**迁移之后**的那一份") {
  ensureTestOps();
  // v1 存的是 count，v2 改名成 keepCount（ADR-0008）。
  // 图里写的键在当前 manifest 里压根不存在，所以这条只有 core 答得出来。
  Json doc = makeGraph({N{"n_src", "test.counted", Json{{"pointCount", 30}}},
                        N{"n_m", "test.migrated", Json{{"count", 7}}}},
                       {E{"n_src.cloud", "n_m.cloud"}});
  doc["nodes"][1]["opVersion"] = "1.0.0";
  const Json row = rowOf(paramsOf(doc), "n_m", "keepCount");
  CHECK(row["value"] == 7);
  CHECK(row["source"] == "explicit");
}

TEST_CASE("params：子图提升参数灌进来的内参是 bound") {
  ensureTestOps();
  // 展开之后，「用户在这个节点上填的」和「外层子图表单灌进来的」在 params 里
  // 长得一模一样 —— source 要分得开这两件事（ADR-0010 F4）。
  Json doc = makeGraph({N{"n_src", "test.counted", Json{{"pointCount", 40}}},
                        N{"s", "sub:clean", Json{{"leaf", Json::array({0.05, 0.05, 0.05})}}}},
                       {E{"n_src.cloud", "s.cloud"}});
  doc["subgraphs"]["clean"] = Json{
      {"name", "去噪"},
      {"nodes", Json::array({
                    // v 的 leaf 由外参绑定，p 的 leaf 是子图定义里自己写死的
                    Json{{"id", "v"}, {"op", "test.thin"}},
                    Json{{"id", "p"},
                         {"op", "test.thin"},
                         {"params", {{"leaf", Json::array({0.01, 0.01, 0.01})}}}},
                })},
      {"edges", Json::array({Json{{"id", "ei"},
                                  {"from", {{"node", "v"}, {"port", "cloud"}}},
                                  {"to", {{"node", "p"}, {"port", "cloud"}}}}})},
      {"inputs", Json::array({Json{{"name", "cloud"},
                                   {"type", "PointCloud"},
                                   {"to", Json::array({Json{{"node", "v"}, {"port", "cloud"}}})}}})},
      {"outputs", Json::array({Json{{"name", "cloud"},
                                    {"type", "PointCloud"},
                                    {"from", {{"node", "p"}, {"port", "cloud"}}}}})},
      {"params", Json::array({Json{{"name", "leaf"},
                                   {"type", "vec3f"},
                                   {"default", Json::array({0.02, 0.02, 0.02})},
                                   {"binds", Json::array({Json{{"node", "v"}, {"param", "leaf"}}})}}})},
  };
  const Json view = paramsOf(doc);

  const Json bound = rowOf(view, "s/v", "leaf");
  CHECK(bound["source"] == "bound");
  CHECK(bound["value"] == Json::array({0.05, 0.05, 0.05}));

  const Json inner = rowOf(view, "s/p", "leaf");
  CHECK(inner["source"] == "explicit");
  CHECK(inner["value"] == Json::array({0.01, 0.01, 0.01}));

  // 子图节点本身不在视图里：展开之后执行器看不见它，params 也不该看见（F1）
  for (const Json& n : view["nodes"]) CHECK(n["node"] != "s");
}

TEST_CASE("params：图不合法时返回诊断数组，未知参数照样是 unknown_param") {
  ensureTestOps();
  const Json doc = makeGraph({N{"n_src", "test.counted", Json{{"nope", 1}}}}, {});
  const Json out = Json::parse(exec::effectiveParamsJson(doc.dump(), {}));
  REQUIRE(out.is_array());
  REQUIRE(out.size() >= 1);
  CHECK(out[0]["code"] == "unknown_param");
  CHECK(out[0]["severity"] == "error");
}

}  // namespace lyflow::test
