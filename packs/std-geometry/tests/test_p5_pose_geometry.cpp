#include "helpers.h"
#include "lyflow/pose.h"
#include <cmath>

namespace {
using namespace lyflow;
using Json = nlohmann::json;
Data p5Poses(const std::vector<std::array<double, 3>>& points, const char* frame = "world",
             poses::Quaternion q = {0, 0, 0, 1}) {
  Json a = Json::array();
  int i = 0;
  for (auto point : points) {
    auto r = poses::make3D("p" + std::to_string(i++), frame, point, q, {.2, .4, .6});
    r.data["classLabel"] = "连接件";
    r.data["pickable"] = true;
    r.data["attributes"] = {{"batch", "A"}};
    a.push_back(r.data);
  }
  return Data::record(poses::make3DList(a, frame));
}
const Json& p5Record(const test::OpCall& c, const char* port) {
  return c.outputs.at(port).asRecord()->data;
}
}

TEST_CASE("P5 pose cyclic axis canonicalization preserves instances and explicit symmetry") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"poses", p5Poses({{1, 2, 3}}, "world", {0, 0, 1, 0})}};
  REQUIRE(c.run("pose.flip_axis_3d").ok);
  const auto& p = p5Record(c, "poses")["items"][0];
  auto r = poses::rotationMatrix(p["rotation"].get<poses::Quaternion>());
  CHECK(r[0] == doctest::Approx(1));
  CHECK(p["position"] == Json::array({1, 2, 3}));
  CHECK(p["classLabel"] == "连接件");
  CHECK(p["attributes"]["batch"] == "A");
  CHECK(p["pickable"] == false);
  CHECK_FALSE(c.run("pose.flip_axis_3d", {{"symmetryOrder", Value::integer(0)}}).ok);
  CHECK(c.outputs.empty());
  CHECK_FALSE(c.run("pose.flip_axis_3d", {{"alignAxis", Value::text("Z")}}).ok);
  for (const char* axis : {"X", "Y", "Z"}) {
    c.inputs = {{"poses", p5Poses({{0, 0, 0}})}};
    REQUIRE(c.run("pose.flip_axis_3d",
                  {{"symmetryAxis", Value::text(axis)},
                   {"alignAxis", Value::text(std::string(axis) == "X" ? "Y" : "X")},
                   {"symmetryOrder", Value::integer(3)}})
                .ok);
  }
}
TEST_CASE("P5 continuous axis rotation handles point direction planar and degeneracy") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"poses", p5Poses({{1, 2, 3}})}};
  CHECK_FALSE(c.run("pose.rotate_axis_3d").ok);
  REQUIRE(c.run("pose.rotate_axis_3d", {{"continuousSymmetry", Value::boolean(true)},
                                        {"reference", Value::vec({0, 1, 8})}})
              .ok);
  auto r =
      poses::rotationMatrix(p5Record(c, "poses")["items"][0]["rotation"].get<poses::Quaternion>());
  CHECK(r[0] == doctest::Approx(0).epsilon(1e-12));
  CHECK(r[3] == doctest::Approx(1));
  CHECK(r[8] == doctest::Approx(1));
  REQUIRE(c.run("pose.rotate_axis_3d", {{"continuousSymmetry", Value::boolean(true)},
                                        {"mode", Value::text("point")},
                                        {"reference", Value::vec({2, 2, 3})},
                                        {"reverse", Value::boolean(true)},
                                        {"useZ", Value::boolean(false)}})
              .ok);
  r = poses::rotationMatrix(p5Record(c, "poses")["items"][0]["rotation"].get<poses::Quaternion>());
  CHECK(r[0] == doctest::Approx(-1));
  for (auto target : {std::vector<double>{0, 0, 0}, std::vector<double>{0, 0, 1}})
    CHECK_FALSE(c.run("pose.rotate_axis_3d", {{"continuousSymmetry", Value::boolean(true)},
                                              {"reference", Value::vec(target)}})
                    .ok);
}
TEST_CASE("P5 comparison uses axis angles not Euler and applies declared cross frame transform") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"a", p5Poses({{0, 0, 0}})}, {"b", p5Poses({{0, 0, 0}}, "other", {0, 0, 1, 0})}};
  CHECK_FALSE(c.run("pose.compare_3d").ok);
  c.inputs["bToA"] = Data::transform(Transform{});
  REQUIRE(c.run("pose.compare_3d").ok);
  CHECK(p5Record(c, "result")["matched"] == false);
  REQUIRE(c.run("pose.compare_3d", {{"symmetryOrder", Value::integer(2)}}).ok);
  CHECK(p5Record(c, "result")["matched"] == true);
  REQUIRE(c.run("pose.compare_3d", {{"continuousSymmetry", Value::boolean(true)}}).ok);
  CHECK(p5Record(c, "result")["matched"] == true);
  CHECK_FALSE(c.run("pose.compare_3d", {{"alarm", Value::boolean(true)}}).ok);
  CHECK(c.status.code == "tolerance_exceeded");
  CHECK(c.outputs.empty());
  c.inputs["b"] = p5Poses({{.002, 0, 0}});
  c.inputs.erase("bToA");
  REQUIRE(c.run("pose.compare_3d").ok);
  CHECK(p5Record(c, "result")["items"][0]["translation"][0] == doctest::Approx(.002));
  c.inputs["a"] = Data::record(poses::make3DList(Json::array()));
  c.inputs["b"] = c.inputs["a"];
  REQUIRE(c.run("pose.compare_3d").ok);
  CHECK(p5Record(c, "result")["items"].empty());
}
TEST_CASE(
    "P5 rigid template distance and proper registration give real source to target residuals") {
  using namespace lyflow;
  const std::vector<std::array<double, 3>> points{{0, 0, 0}, {.1, 0, 0}, {0, .2, 0}, {0, 0, .3}};
  test::OpCall c;
  c.inputs = {{"poses", p5Poses(points, "target")}};
  REQUIRE(c.run("pose.rigid_template", {{"tolerance", Value::number(1e-6)}}).ok);
  auto templ = c.outputs.at("template");
  std::vector<std::array<double, 3>> moved;
  for (auto a : points)
    moved.push_back({-a[1] + 1, a[0] + 2, a[2] + 3});
  c.inputs = {{"template", templ}, {"poses", p5Poses(moved, "source")}};
  REQUIRE(c.run("pose.rigid_check").ok);
  CHECK(p5Record(c, "result")["accepted"] == true);
  CHECK(c.outputs.count("transform") == 0);
  REQUIRE(c.run("pose.rigid_check", {{"mode", Value::text("registration")}}).ok);
  CHECK(p5Record(c, "result")["accepted"] == true);
  CHECK(p5Record(c, "result")["observable"] == true);
  CHECK(c.outputs.count("transform") == 0);
  CHECK(p5Record(c, "result")["transformAvailable"] == true);
  REQUIRE(c.run("pose.rigid_register").ok);
  const auto* t = c.outputs.at("transform").asTransform();
  CHECK(p5Record(c, "result")["transform"] == Json(t->m));
  CHECK(t->m[1] == doctest::Approx(1));
  CHECK(t->m[4] == doctest::Approx(-1));
  CHECK(t->m[3] == doctest::Approx(-2));
  CHECK(t->m[7] == doctest::Approx(1));
  CHECK(t->m[11] == doctest::Approx(-3));
  moved[3][2] += .01;
  c.inputs["poses"] = p5Poses(moved, "source");
  REQUIRE(c.run("pose.rigid_check").ok);
  CHECK(p5Record(c, "result")["accepted"] == false);
  REQUIRE(c.run("pose.rigid_check", {{"mode", Value::text("registration")}}).ok);
  CHECK(p5Record(c, "result")["accepted"] == false);
  REQUIRE(c.run("pose.rigid_register").ok);
  CHECK(p5Record(c, "result")["accepted"] == false);
  CHECK(p5Record(c, "result")["transformAvailable"] == true);
  CHECK(c.outputs.at("transform").asTransform() != nullptr);
  CHECK_FALSE(c.run("pose.rigid_check", {{"alarm", Value::boolean(true)}}).ok);
  CHECK(c.outputs.empty());
}
TEST_CASE(
    "P5 rigid registration rejects reflection and unobservable supports without fabricated transform") {
  using namespace lyflow;
  test::OpCall c;
  c.inputs = {{"poses", p5Poses({{0, 0, 0}, {1, 0, 0}, {0, 2, 0}, {0, 0, 3}})}};
  REQUIRE(c.run("pose.rigid_template").ok);
  auto templ = c.outputs.at("template");
  c.inputs = {{"template", templ},
              {"poses", p5Poses({{0, 0, 0}, {-1, 0, 0}, {0, 2, 0}, {0, 0, 3}})}};
  REQUIRE(c.run("pose.rigid_check", {{"mode", Value::text("registration")}}).ok);
  CHECK(p5Record(c, "result")["accepted"] == false);
  c.inputs = {{"poses", p5Poses({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}})}};
  REQUIRE(c.run("pose.rigid_template").ok);
  templ = c.outputs.at("template");
  c.inputs["template"] = templ;
  REQUIRE(c.run("pose.rigid_check", {{"mode", Value::text("registration")}}).ok);
  CHECK(p5Record(c, "result")["observable"] == false);
  CHECK(p5Record(c, "result")["maximumError"].is_null());
  CHECK(p5Record(c, "result")["residualCount"] == 0);
  CHECK(c.outputs.count("transform") == 0);
  CHECK(p5Record(c, "result")["transformAvailable"] == false);
  CHECK(p5Record(c, "result")["transform"].is_null());
  CHECK(c.run("pose.rigid_register").code == "unobservable");
  CHECK(c.outputs.empty());
}
TEST_CASE("P5 geometric lifting embeds true point segment circle and metadata preserving poses") {
  using namespace lyflow;
  test::OpCall c;
  Point2D point{{10, 20}, Unit2D::Pixel};
  c.inputs = {{"point", Data::point2d(point)}};
  REQUIRE(
      c.run("geometry.extend_2d", {{"newAxis", Value::text("Y")}, {"offset", Value::number(.5)}})
          .ok);
  CHECK(p5Record(c, "features")["items"][0]["position"] == Json::array({.01, .5, .02}));
  Line2D line;
  line.hasSegment = true;
  line.start[0] = 1;
  line.end[1] = 2;
  c.inputs = {{"line", Data::line2d(line)}};
  REQUIRE(c.run("geometry.extend_2d").ok);
  CHECK(p5Record(c, "features")["items"][0]["end"] == Json::array({0, 2, 0}));
  Circle2D circle{{1, 2}, 3, Unit2D::Meter};
  c.inputs = {{"circle", Data::circle2d(circle)}};
  REQUIRE(c.run("geometry.extend_2d", {{"newAxis", Value::text("X")}}).ok);
  CHECK(p5Record(c, "features")["items"][0]["normal"] == Json::array({1, 0, 0}));
  CHECK(p5Record(c, "features")["items"][0]["radius"] == 3);
  auto pose = poses::make2D("件", "image", "px", {10, 20}, 0, {30, 40});
  pose.data["classLabel"] = "连接件";
  pose.data["pickable"] = true;
  c.inputs = {{"poses", Data::record(pose)}};
  REQUIRE(c.run("geometry.extend_2d").ok);
  const auto& a = p5Record(c, "poses")["items"][0];
  CHECK(a["position"] == Json::array({.01, .02, 0}));
  CHECK(a["classLabel"] == "连接件");
  CHECK(a["pickable"] == false);
  c.inputs["point"] = Data::point2d(point);
  CHECK_FALSE(c.run("geometry.extend_2d").ok);
  CHECK(c.outputs.empty());
}
TEST_CASE(
    "P5 geometry collections lift every feature preserve identities and reject unknown modes") {
  using namespace lyflow;
  test::OpCall c;
  Json points =
      Json::array({{{"id", "点"},
                    {"type", "point"},
                    {"position", {10, 20}},
                    {"attributes", {{"nullable", nullptr}}}},
                   {{"id", "线"},
                    {"type", "line"},
                    {"position", {1, 2}},
                    {"direction", {1, 1}},
                    {"start", {1, 2}},
                    {"end", {3, 4}}},
                   {{"id", "圆"}, {"type", "circle"}, {"position", {5, 6}}, {"radius", 2}}});
  auto input =
      Record{"geometry.Features2D.v1", {{"frame", "image"}, {"unit", "px"}, {"items", points}}};
  c.inputs = {{"features", Data::record(input)}};
  REQUIRE(c.run("geometry.extend_2d", {{"newAxis", Value::text("Y")}}).ok);
  const auto& items = p5Record(c, "features")["items"];
  CHECK(items.size() == 3);
  CHECK(items[0]["id"] == "点");
  CHECK(items[0]["position"] == Json::array({.01, 0, .02}));
  CHECK(items[0]["attributes"]["nullable"].is_null());
  CHECK(items[1]["end"] == Json::array({.003, 0, .004}));
  CHECK(items[2]["radius"].get<double>() == doctest::Approx(.002));
  CHECK(items[2]["normal"] == Json::array({0, -1, 0}));
  input.data["items"][2]["type"] = "ellipse";
  c.inputs["features"] = Data::record(input);
  CHECK_FALSE(c.run("geometry.extend_2d").ok);
  CHECK(c.outputs.empty());
}
TEST_CASE("P5 geometry finite input budgets and cancellation are atomic") {
  using namespace lyflow;
  struct Cancel final : ExecContext {
    mutable int polls = 0;
    int stop;
    std::filesystem::path base;
    explicit Cancel(int v) : stop(v) {}
    bool cancelled() const override { return ++polls >= stop; }
    void progress(float, std::string_view) override {}
    void log(LogLevel, std::string) override {}
    const std::filesystem::path& baseDir() const override { return base; }
    int threadBudget() const override { return 1; }
  };
  const auto data = p5Poses({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
  test::OpCall creator;
  creator.inputs = {{"poses", data}};
  REQUIRE(creator.run("pose.rigid_template").ok);
  auto templ = creator.outputs.at("template");
  for (const char* name :
       {"pose.flip_axis_3d", "pose.rotate_axis_3d", "pose.compare_3d", "pose.rigid_template",
        "pose.rigid_check", "pose.rigid_register", "geometry.extend_2d"})
    for (int stop : {1, 2}) {
      CAPTURE(name);
      CAPTURE(stop);
      const auto* op = ensureRegistry().find(name);
      REQUIRE(op);
      ParamMap params;
      for (const auto& v : op->params)
        params[v.name] = v.def;
      params["continuousSymmetry"] = Value::boolean(true);
      std::unordered_map<std::string, Data> values{
          {"poses", data}, {"a", data}, {"b", data}, {"template", templ}};
      if (std::string(name) == "geometry.extend_2d")
        values = {{"point", Data::point2d(Point2D{})}};
      Inputs inputs(values);
      std::unordered_map<std::string, Data> output;
      Outputs outputs(output);
      Cancel ctx(stop);
      ParamView p(params, ctx.base);
      CHECK(op->compute(inputs, p, outputs, ctx).code == "cancelled");
      CHECK(output.empty());
    }
  std::vector<std::array<double, 3>> many(257, {0, 0, 0});
  test::OpCall c;
  c.inputs = {{"poses", p5Poses(many)}};
  CHECK(c.run("pose.flip_axis_3d", {{"symmetryOrder", Value::integer(4096)}}).code ==
        "resource_limit");
  CHECK(c.outputs.empty());
}

TEST_CASE("P5 pose self comparison retains tight axis tolerances without acos amplification") {
  using namespace lyflow;
  for (int i = 1; i <= 20; ++i) {
    auto pose = poses::make3D(
        "same", "world", {0, 0, 0},
        {std::sin(i * .19), std::cos(i * .17), std::sin(i * .13), std::cos(i * .11)}, {1, 1, 1});
    auto data = Data::record(pose);
    test::OpCall c;
    c.inputs = {{"a", data}, {"b", data}};
    REQUIRE(c.run("pose.compare_3d", {{"angleTolerance", Value::number(1e-10)},
                                      {"axisTolerance", Value::vec({1e-10, 1e-10, 1e-10})}})
                .ok);
    CHECK(p5Record(c, "result")["matched"] == true);
  }
  auto first = poses::make3D("near", "world", {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}),
       second = poses::make3D("near", "world", {0, 0, 0}, {0, 0, std::sin(5e-9), std::cos(5e-9)},
                              {1, 1, 1});
  test::OpCall c;
  c.inputs = {{"a", Data::record(first)}, {"b", Data::record(second)}};
  REQUIRE(c.run("pose.compare_3d", {{"angleTolerance", Value::number(1e-10)},
                                    {"axisTolerance", Value::vec({1e-10, 1e-10, 1e-10})}})
              .ok);
  CHECK(p5Record(c, "result")["matched"] == false);
  CHECK(p5Record(c, "result")["items"][0]["axisAngles"][0].get<double>() ==
        doctest::Approx(1e-8).epsilon(1e-6));
}

TEST_CASE(
    "P5 scheduler rigid checks preserve distances and explicitly reject unavailable transforms") {
  using namespace lyflow;
  const auto points = p5Poses({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}});
  test::OpCall creator;
  creator.inputs = {{"poses", points}};
  REQUIRE(creator.run("pose.rigid_template").ok);
  const auto templ = creator.outputs.at("template");
  for (const char* mode : {"distances", "registration"}) {
    CAPTURE(mode);
    auto graph = test::makeGraph({{"check", "pose.rigid_check", {{"mode", mode}}}}, {});
    test::Session session(graph, {}, {}, false, 1, true,
                          {{"check", "poses", points}, {"check", "template", templ}});
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data result, accepted, missing;
    auto& store = exec::ResultStore::instance();
    REQUIRE(store.get(log.runId, "check", "result", result));
    REQUIRE(store.get(log.runId, "check", "accepted", accepted));
    const auto& data = result.asRecord()->data;
    CHECK(data.at("transformAvailable") == false);
    CHECK(data.at("transform").is_null());
    CHECK_FALSE(store.get(log.runId, "check", "transform", missing));
    if (std::string(mode) == "distances") {
      CHECK(data.at("accepted") == true);
      CHECK(data.at("maximumError") == 0);
    } else {
      CHECK(data.at("accepted") == false);
      CHECK(data.at("observable") == false);
      CHECK(data.at("maximumError").is_null());
    }
  }
  auto graph = test::makeGraph({{"fit", "pose.rigid_register"}}, {});
  test::Session session(graph, {}, {}, false, 1, true,
                        {{"fit", "poses", points}, {"fit", "template", templ}});
  const auto& log = session.wait();
  CHECK(log.runStatus() == "error");
  Data result;
  CHECK_FALSE(exec::ResultStore::instance().get(log.runId, "fit", "transform", result));
  bool unobservable = false;
  for (const auto& event : log.events)
    if (event.contains("error"))
      unobservable = unobservable || event.at("error").value("code", "") == "unobservable";
  CHECK(unobservable);
}

TEST_CASE("P5 scheduler all five lifting inputs publish explicit poses and geometry collections") {
  using namespace lyflow;
  static const bool registered = [] {
    OperatorDesc source;
    source.id = "test.p5_geometry_record_source";
    source.version = "1.0.0";
    source.label = "P5几何记录夹具";
    source.category = "测试/夹具";
    source.outputs = {{"record", "Record", "Fixture", "Injected record fixture", true}};
    source.compute = [](const Inputs&, const ParamView&, Outputs&, ExecContext&) {
      return Status::Error(Phase::Execute, "missing_fixture", "测试源必须显式注入记录");
    };
    ensureRegistry().addOperator(std::move(source));
    return true;
  }();
  REQUIRE(registered);
  Line2D line;
  line.dir[0] = 1;
  const std::vector<std::pair<std::string, Data>> inputs{
      {"point", Data::point2d(Point2D{{1, 2}, Unit2D::Meter})},
      {"line", Data::line2d(line)},
      {"circle", Data::circle2d(Circle2D{{1, 2}, 3, Unit2D::Meter})},
      {"poses", Data::record(poses::make2D("pose", "image", "m", {1, 2}, 0, {1, 1}))},
      {"features",
       Data::record(
           {"geometry.Features2D.v1",
            {{"frame", "image"},
             {"unit", "m"},
             {"items",
              Json::array({{{"id", "feature"}, {"type", "point"}, {"position", {1, 2}}}})}}})}};
  for (const auto& input : inputs) {
    CAPTURE(input.first);
    auto graph = test::makeGraph({{"lift", "geometry.extend_2d"}}, {});
    std::vector<exec::InjectedInput> injected{{"lift", input.first, input.second}};
    if (input.first == "poses") {
      graph = test::makeGraph({{"source",
                                "pose.construct_2d",
                                {{"id", "pose"},
                                 {"frame", "image"},
                                 {"unit", "m"},
                                 {"position", {1, 2}},
                                 {"size", {1, 1}}}},
                               {"lift", "geometry.extend_2d"}},
                              {{"source.pose", "lift.poses"}});
      injected.clear();
    } else if (input.first == "features") {
      graph = test::makeGraph(
          {{"source", "test.p5_geometry_record_source"}, {"lift", "geometry.extend_2d"}},
          {{"source.record", "lift.features"}});
      injected = {{"source", "record", input.second}};
    }
    test::Session session(graph, {}, {}, false, 1, true, std::move(injected));
    const auto& log = session.wait();
    REQUIRE_MESSAGE(log.runStatus() == "ok", Json(log.events).dump());
    Data posesResult, features;
    auto& store = exec::ResultStore::instance();
    REQUIRE(store.get(log.runId, "lift", "poses", posesResult));
    REQUIRE(store.get(log.runId, "lift", "features", features));
    std::string why;
    REQUIRE_MESSAGE(poses::validate(*posesResult.asRecord(), why), why);
    CHECK(features.asRecord()->type == "geometry.Features3D.v1");
    CHECK(posesResult.asRecord()->data.at("items").size() == (input.first == "poses" ? 1 : 0));
    CHECK(features.asRecord()->data.at("items").size() == (input.first == "poses" ? 0 : 1));
  }
}
