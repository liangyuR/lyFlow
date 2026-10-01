// 预览时图像按比例缩小与像素量的换算（docs/large-image-plan.md E4–E9，ADR-0028）。
// 换算规则本身直接调 exec/pixel_scale 的函数；端到端那一条走真的执行器（源头缩小 → 下游换算 → 出来换回）。
#include <doctest/doctest.h>

#include <mutex>

#include "exec/pixel_scale.h"
#include "exec/plan.h"
#include "helpers.h"
#include "test_ops.h"

using namespace lyflow;
using namespace lyflow::test;

namespace {

/// 收到什么就报什么的探针：像素参数、图的尺寸与比例、可选的像素圆输入都写进 seen；
/// 另出一个像素圆（center = radius = len）、一个 px² 的量测（输入的像素数）与一张图（mode = size 时是绝对尺寸）。
Status probeCompute(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Image* img = in.get("image").asImage();
  Record seen;
  seen.type = "test.Seen";
  seen.data["len"] = p.number("len");
  seen.data["k"] = p.integer("k");
  const auto roi = p.vec4("roi");
  seen.data["roi"] = {roi[0], roi[1], roi[2], roi[3]};
  seen.data["size"] = p.integer("size");
  seen.data["imageWidth"] = img->width;
  seen.data["imageScale"] = img->scale;
  if (in.has("near")) {
    const Circle2D* c = in.get("near").asCircle2D();
    seen.data["near"] = {c->center[0], c->center[1], c->radius};
  }
  if (in.has("other")) {
    const Image* other = in.get("other").asImage();
    seen.data["otherWidth"] = other->width;
    seen.data["otherScale"] = other->scale;
  }
  out.set("seen", Data::record(seen));

  Circle2D circle;
  circle.center[0] = circle.center[1] = circle.radius = static_cast<float>(p.number("len"));
  circle.unit = Unit2D::Pixel;
  out.set("circle", Data::circle2d(circle));

  Measurement area;
  area.ok = true;
  area.unit = "px²";
  area.value = static_cast<double>(img->pixelCount());
  out.set("area", Data::measurement(area));

  const bool sized = p.choice("mode") == "size";
  const auto side = static_cast<std::int32_t>(p.integer("size"));
  out.set("image", Data::image(sized ? Image::allocate(side, side, 1, PixelDepth::U8)
                                     : Image::allocate(img->width, img->height, 1, PixelDepth::U8)));
  return Status::Ok();
}

const OperatorDesc& probeOp() {
  static std::once_flag once;
  std::call_once(once, [] {
    ensureTestOps();
    OperatorDesc op;
    op.id = "test.px_probe";
    op.version = "1.0.0";
    op.label = "像素探针";
    op.category = "Test";
    op.inputs = {Port{"image", "Image", "Image", "", true}, Port{"near", "Circle2D", "Near", "", false},
                 Port{"other", "Image", "Other", "", false}};
    op.outputs = {Port{"seen", "Record", "Seen", "", true}, Port{"circle", "Circle2D", "Circle", "", true},
                  Port{"area", "Measurement", "Area", "", true}, Port{"image", "Image", "Image", "", true}};
    Param len;
    len.name = "len";
    len.type = ParamType::Float;
    len.label = "len";
    len.def = Value::number(10);
    len.min = 0.0;
    len.unit = "px";
    Param k;
    k.name = "k";
    k.type = ParamType::Int;
    k.label = "k";
    k.def = Value::integer(5);
    k.min = 1;
    k.max = 99;
    k.step = 2;
    k.unit = "px";
    Param roi;
    roi.name = "roi";
    roi.type = ParamType::Vec4f;
    roi.label = "roi";
    roi.def = Value::vec({8, 16, 400, 800});
    roi.unit = "px";
    Param mode;
    mode.name = "mode";
    mode.type = ParamType::Enum;
    mode.label = "mode";
    mode.def = Value::text("same");
    mode.options = {EnumOption{"same", "same", ""}, EnumOption{"size", "size", ""}};
    Param size;
    size.name = "size";
    size.type = ParamType::Int;
    size.label = "size";
    size.def = Value::integer(64);
    size.min = 1;
    size.max = 8192;
    size.unit = "px";
    size.absolute = true;
    size.visibleWhen.param = "mode";
    size.visibleWhen.eq = Value::text("size");
    op.params = {len, k, roi, mode, size};
    op.capabilities = {false, false, true};
    op.compute = &probeCompute;
    ensureRegistry().addOperator(std::move(op));
  });
  return *ensureRegistry().find("test.px_probe");
}

/// 先把事件留住：nodeEvent 返回的是临时 Json，range-for 里直接对它取下标会悬空（M4 记过同一个坑）。
Json outputValue(const RunLog& log, const std::string& node, const std::string& port) {
  const Json done = log.nodeEvent(node, "done");
  const auto stats = done.find("stats");
  if (stats == done.end()) return Json();
  for (const Json& o : stats->value("outputs", Json::array())) {
    if (o.value("port", "") == port) return o.value("value", Json());
  }
  return Json();
}

RunLog runIn(const Json& doc, exec::RunMode mode, std::uint32_t budgetMs = 0, bool keepCache = false) {
  static int counter = 0;
  if (!keepCache) exec::ResultStore::instance().clear();
  exec::RunOptions options;
  options.runId = "pixel-scale-" + std::to_string(counter++);
  options.mode = mode;
  options.previewBudgetMs = budgetMs;
  RunLog log;
  log.runId = options.runId;
  exec::Run run(doc.dump(), options, &detail::collect, &log);
  run.join();
  return log;
}

}  // namespace

TEST_CASE("previewLevel：缩到不超过上限的最小 2 的幂，与 shrinkImage 同一种向上取整") {
  const auto gray = [](std::int32_t w, std::int32_t h) { return Image::allocate(w, h, 1, PixelDepth::U8); };
  CHECK(exec::previewLevel(gray(2048, 2048), exec::kPreviewMaxPixels) == 0);  // 正好 4 MP
  CHECK(exec::previewLevel(gray(2049, 2048), exec::kPreviewMaxPixels) == 1);
  CHECK(exec::previewLevel(gray(3, 3), 1) == 2);  // 3 → 2 → 1
  CHECK(exec::previewLevel(Image{}, exec::kPreviewMaxPixels) == 0);

  const Image camera = gray(5472, 3648);  // 20 MP → 1368 × 912
  const unsigned level = exec::previewLevel(camera, exec::kPreviewMaxPixels);
  const Image small = shrinkImage(camera, level);
  CHECK(level == 2);
  CHECK(small.pixelCount() <= exec::kPreviewMaxPixels);
  CHECK(small.scale == 4);
}

TEST_CASE("像素参数 ÷ s：浮点直接除、整数落回 min + n × step 再夹到边界、vec 逐分量、绝对尺寸不换") {
  const OperatorDesc& op = probeOp();
  ParamMap params{{"len", Value::number(10)},
                  {"k", Value::integer(5)},
                  {"roi", Value::vec({8, 16, 400, 800})},
                  {"mode", Value::text("size")},
                  {"size", Value::integer(640)}};

  SUBCASE("s = 2") {
    const ParamMap out = exec::scalePixelParams(op, params, 2);
    CHECK(out.at("len").floatValue() == doctest::Approx(5));
    CHECK(out.at("k").intValue() == 3);  // 2.5 落回奇数
    CHECK(out.at("roi").vecValue() == std::vector<double>{4, 8, 200, 400});
    CHECK(out.at("size").intValue() == 640);  // 绝对尺寸
    CHECK(out.at("mode").stringValue() == "size");
  }
  SUBCASE("s = 4：核缩到 min 还是奇数") {
    const ParamMap out = exec::scalePixelParams(op, params, 4);
    CHECK(out.at("k").intValue() == 1);
    CHECK(out.at("len").floatValue() == doctest::Approx(2.5));
  }
  SUBCASE("s = 1 原样") {
    CHECK(exec::canonicalParamsJson(exec::scalePixelParams(op, params, 1)) ==
          exec::canonicalParamsJson(params));
  }

  CHECK(exec::hasActiveAbsoluteSize(op, params));  // mode = size：size 生效
  params["mode"] = Value::text("same");
  CHECK_FALSE(exec::hasActiveAbsoluteSize(op, params));  // size 被 visibleWhen 藏起来了
}

TEST_CASE("像素几何与像素量测按比例换算：米制的、别的单位的原样，Bundle 逐字段") {
  const auto box = [](Unit2D unit) {
    Box2D b;
    b.min[0] = 1;
    b.min[1] = 2;
    b.max[0] = 3;
    b.max[1] = 4;
    b.unit = unit;
    return Data::box2d(b);
  };
  // 换算出来的是一份新 Data：先留住它，as* 拿的是它里面的指针
  const Data pxBox = exec::scalePixelData(box(Unit2D::Pixel), 2);
  const Box2D* px = pxBox.asBox2D();
  CHECK(std::vector<float>{px->min[0], px->min[1], px->max[0], px->max[1]} ==
        std::vector<float>{2, 4, 6, 8});
  const Data meterBox = exec::scalePixelData(box(Unit2D::Meter), 2);
  CHECK(meterBox.asBox2D()->max[0] == 3);

  Line2D l;
  l.point[0] = 1;
  l.dir[0] = 0.6f;
  l.dir[1] = 0.8f;
  l.hasSegment = true;
  l.end[1] = 5;
  l.unit = Unit2D::Pixel;
  const Data line = exec::scalePixelData(Data::line2d(l), 0.5);
  const Line2D* sl = line.asLine2D();
  CHECK(sl->point[0] == 0.5f);
  CHECK(sl->end[1] == 2.5f);
  CHECK(sl->dir[1] == 0.8f);  // 方向是单位向量，不变

  Circle2D c;
  c.center[0] = 2;
  c.radius = 3;
  c.unit = Unit2D::Pixel;
  const Data circle = exec::scalePixelData(Data::circle2d(c), 2);
  const Circle2D* sc = circle.asCircle2D();
  CHECK(sc->center[0] == 4);
  CHECK(sc->radius == 6);

  const auto measured = [](const char* unit) {
    Measurement m;
    m.value = 3;
    m.ok = true;
    m.unit = unit;
    m.hasLimits = true;
    m.nominal = 3;
    m.upper = 4;
    m.lower = 2;
    return Data::measurement(m);
  };
  const Data lengthData = exec::scalePixelData(measured("px"), 2);
  const Measurement* length = lengthData.asMeasurement();
  CHECK(std::vector<double>{length->value, length->nominal, length->upper, length->lower} ==
        std::vector<double>{6, 6, 8, 4});
  const Data area = exec::scalePixelData(measured("px²"), 2);
  const Data mm = exec::scalePixelData(measured("mm"), 2);
  CHECK(area.asMeasurement()->value == 12);
  CHECK(mm.asMeasurement()->value == 3);

  Bundle b("test.PxPair");
  b.set("box", box(Unit2D::Pixel));
  b.set("note", measured("mm"));
  const Data bundle = exec::scalePixelData(Data::bundle(b), 2);
  const Bundle* sb = bundle.asBundle();
  CHECK(sb->field("box")->asBox2D()->max[1] == 8);
  CHECK(sb->field("note")->asMeasurement()->value == 3);
}

TEST_CASE("预览时源头的大图按 2 的幂缩小，下游在小图上算、像素量出来换回原图坐标（ADR-0028）") {
  probeOp();
  // src 4100 × 2050 = 8.4 MP → 缩一级到 2050 × 1025（scale 2）
  // a：普通的像素参数；b：resize 到绝对尺寸那一类（mode = size）；
  // c 吃 b 的图（回到原图比例）和 a 的圆；d 吃 a 的图（还是 scale 2）和 a 的圆；
  // e 吃 a 的图（scale 2）与 b 的图（scale 1）：比例不一样的两张图进同一个节点
  const Json doc = makeGraph(
      {N{"src", "test.make_image", Json{{"width", 4100}, {"height", 2050}, {"channels", 1}}},
       N{"a", "test.px_probe", Json{{"len", 10}, {"k", 5}}},
       N{"b", "test.px_probe", Json{{"mode", "size"}, {"size", 640}}},
       N{"c", "test.px_probe", Json::object()},
       N{"d", "test.px_probe", Json::object()},
       N{"e", "test.px_probe", Json::object()}},
      {E{"src.image", "a.image"}, E{"a.image", "b.image"}, E{"b.image", "c.image"},
       E{"a.circle", "c.near"}, E{"a.image", "d.image"}, E{"a.circle", "d.near"},
       E{"a.image", "e.image"}, E{"b.image", "e.other"}});

  SUBCASE("预览") {
    const RunLog log = runIn(doc, exec::RunMode::Preview, /*budgetMs=*/1);
    REQUIRE(log.runStatus() == "ok");
    const Json src = outputValue(log, "src", "image");
    CHECK(Json{src["width"], src["height"], src["scale"]} == Json{2050, 1025, 2});

    const Json a = outputValue(log, "a", "seen")["data"];
    CHECK(a == Json{{"len", 5.0}, {"k", 3}, {"roi", {4, 8, 200, 400}}, {"size", 64},
                    {"imageWidth", 2050}, {"imageScale", 2}});
    // 圆在小图上是 5，出来换回原图坐标是 10；面积按 s² 换回，正好等于原图的像素数
    const Json circle = outputValue(log, "a", "circle");
    CHECK(Json{circle["center"][0], circle["radius"]} == Json{10.0, 10.0});
    CHECK(outputValue(log, "a", "area")["value"] == 4100.0 * 2050.0);
    CHECK(outputValue(log, "a", "image")["scale"] == 2);

    // 绝对尺寸：不换算，输出回到原图比例（valueJson 不写 scale）
    CHECK(outputValue(log, "b", "seen")["data"]["size"] == 640);
    CHECK_FALSE(outputValue(log, "b", "image").contains("scale"));
    const Json c = outputValue(log, "c", "seen")["data"];
    CHECK(Json{c["imageScale"], c["len"], c["near"]} == Json{1, 10.0, {10, 10, 10}});

    // 连线上的像素圆进 d 时换算到它那张 scale 2 的图上
    const Json d = outputValue(log, "d", "seen")["data"];
    CHECK(Json{d["imageScale"], d["near"]} == Json{2, {5, 5, 5}});

    // 比例不一样的两张图（review 修正）：scale 1 的那张先缩到 scale 2 再进 compute，两张才对得上。
    // 修前原样交进去 —— 例如推理掩膜 resize 回原图尺寸再与预览缩小过的原图一起进 region_stats，报「不一样大」
    const Json e = outputValue(log, "e", "seen")["data"];
    CHECK(Json{e["imageScale"], e["otherWidth"], e["otherScale"]} == Json{2, 320, 2});

    // 超预算的提示：点名最慢的节点；没抽稀点云就不提「降低预览点数」
    std::string warn;
    for (const Json& e : log.ofKind("log")) {
      if (e.value("level", "") == "warn") warn = e.value("message", "");
    }
    CHECK(warn.find("最慢的是 ") != std::string::npos);
    CHECK(warn.find("图像已按 1/2 预览") != std::string::npos);
    CHECK(warn.find("降低预览点数") == std::string::npos);

    // 再拖一下 a 的参数：源头这回命中预览缓存（拖参数时除了第一次都是这样），提示里照样写着缩过图（review 修正）
    Json dragged = doc;
    dragged["nodes"][1]["params"]["len"] = 12;
    const RunLog again = runIn(dragged, exec::RunMode::Preview, /*budgetMs=*/1, /*keepCache=*/true);
    REQUIRE(again.finalState("src") == "skipped");
    std::string warnAgain;
    for (const Json& ev : again.ofKind("log")) {
      if (ev.value("level", "") == "warn") warnAgain = ev.value("message", "");
    }
    CHECK(warnAgain.find("图像已按 1/2 预览") != std::string::npos);
  }
  SUBCASE("正式运行：一切照原图") {
    const RunLog log = runIn(doc, exec::RunMode::Full);
    REQUIRE(log.runStatus() == "ok");
    CHECK_FALSE(outputValue(log, "src", "image").contains("scale"));
    const Json a = outputValue(log, "a", "seen")["data"];
    CHECK(Json{a["len"], a["k"], a["imageWidth"], a["imageScale"]} == Json{10.0, 5, 4100, 1});
    CHECK(outputValue(log, "a", "area")["value"] == 4100.0 * 2050.0);
    CHECK(outputValue(log, "d", "seen")["data"]["near"] == Json{10, 10, 10});
  }
}
