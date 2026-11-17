#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

#include "helpers.h"
#include "exec/plan.h"
#include "lyflow/profile.h"
#include "lyflow_profile/context.h"
#include "ops/profile_scan.h"
#include "ops/profile_scene.h"
#include "ops/profile_feature_bundle.h"
#ifdef LYFLOW_GAP_IMPORT
#include "profile_graph_test_support.h"
#endif

using namespace lyflow;
namespace scene = lyflow::packs::gap::profile_scene;
namespace {
using Json = nlohmann::json;
const SnippetDesc& snippet(const std::string& id) {
  for (const auto& entry : ensureRegistry().snippets())
    if (entry.id == id)
      return entry;
  FAIL(id);
  throw std::logic_error(id);
}
Data profile(bool right) {
  PointCloud cloud;
  std::vector<std::uint8_t> validity;
  std::vector<std::uint32_t> indices;
  for (int i = 0; i < 200; ++i) {
    const float x = (right ? .002F : -.022F) + .0001F * i;
    cloud.push(x, 0, right ? .1705F : .17F);
    validity.push_back(1);
    indices.push_back(static_cast<std::uint32_t>(i));
  }
  Profile result;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(std::move(cloud)),
                      {1, "asset-sensor", "m", "XZ", "right", "acquisition",
                       right ? "asset-right" : "asset-left", 200},
                      std::move(validity), std::move(indices), result, why));
  return Data::profile(std::move(result));
}
struct Location {
  Data scan, rois;
  Location() {
    test::StubContext ctx;
    scene::ReadScanOptions options;
    options.measurementFrame = "asset-measurement";
    REQUIRE(scene::buildReadScan(profile(false), profile(true), options, scan, ctx).ok);
    scene::ScanView view;
    std::string why;
    REQUIRE(scene::readScanPair(scan, view, why));
    scene::RoiFields fields;
    const float bounds[4][4] = {{-.023F, .169F, -.002F, .172F},
                                {.002F, .169F, .023F, .172F},
                                {-.003F, .169F, -.001F, .172F},
                                {.001F, .169F, .003F, .172F}};
    for (std::size_t i = 0; i < 4; ++i) {
      fields.boxes[i] =
          Data::box2d(Box2D{{bounds[i][0], bounds[i][1]}, {bounds[i][2], bounds[i][3]}});
      fields.contexts[i] =
          Data::record(profile_contract::geometryContext(view.merged->asProfile()->descriptor));
    }
    fields.info = {
        scene::kRoiInfoType,
        {{"schemaVersion", 3},
         {"source", "config"},
         {"datumSide", "left"},
         {"alignment", nullptr},
         {"overallMm", nullptr},
         {"cropStatus", nullptr},
         {"processing",
          {{"method", "assemble"}, {"status", "complete"}, {"details", Json::object()}}}}};
    REQUIRE(scene::makeRoiSet(fields, rois, why));
  }
};
Json closed(const SnippetDesc& entry, const Location& location) {
  auto& registry = Registry::instance();
  OperatorDesc source;
  source.id = "test.gap_asset_source";
  source.version = "1.0.0";
  source.label = "test.gap_asset_source";
  source.outputs = {{"scan", "Bundle<gap.ScanPair.v3>", "Scan", "", true},
                    {"rois", "Bundle<gap.RoiSet.v3>", "ROIs", "", true}};
  source.capabilities = {false, false, true};
  source.compute = [scan = location.scan, rois = location.rois](const Inputs&, const ParamView&,
                                                                Outputs& out, ExecContext&) {
    out.set("scan", scan);
    out.set("rois", rois);
    return Status::Ok();
  };
  registry.addOperator(std::move(source));
  auto graph = test::makeGraph({}, {});
  graph["nodes"] = entry.body.at("nodes");
  graph["edges"] = entry.body.at("edges");
  graph["nodes"].push_back({{"id", "assetSource"},
                            {"op", "test.gap_asset_source"},
                            {"opVersion", "1.0.0"},
                            {"params", Json::object()}});
  int i = 0;
  for (auto& edge : graph["edges"])
    edge["id"] = "asset-edge-" + std::to_string(i++);
  for (const auto& port : entry.body.at("ports").at("inputs"))
    graph["edges"].push_back({{"id", "asset-edge-" + std::to_string(i++)},
                              {"from", {{"node", "assetSource"}, {"port", port.at("port")}}},
                              {"to", {{"node", port.at("node")}, {"port", port.at("port")}}}});
  return graph;
}
}

TEST_CASE("native gap assets pin executable versions and expose only available capabilities") {
  const auto& registry = ensureRegistry();
  CHECK(registry.validate().empty());
  for (const char* retired :
       {"gap.fit_line", "gap.selected_point", "gap.nearest_to_line", "gap.drop_non_finite",
        "gap.to_measurement_frame", "gap.flush", "gap.gap", "gap.point_offset"}) {
    CHECK(registry.find(retired) == nullptr);
    CHECK(registry.retiredOperatorHint(retired) != nullptr);
  }
  std::set<std::string> actual;
  for (const auto& entry : registry.snippets()) {
    if (entry.id.rfind("gap.", 0) != 0)
      continue;
    actual.insert(entry.id);
    CHECK(entry.parseError.empty());
    CHECK(entry.pack.rfind("solution-gap@", 0) == 0);
    for (const auto& node : entry.body.at("nodes")) {
      CAPTURE(entry.id);
      CAPTURE(node);
      const auto* op = registry.find(node.at("op").get<std::string>());
      REQUIRE(op);
      CHECK(node.at("opVersion") == op->version);
    }
  }
  std::set<std::string> expected{"gap.flush_line_end", "gap.flush_selected_point",
                                 "gap.gap_circles", "gap.backup_camera", "gap.measure_skeleton"};
#if defined(LYFLOW_GAP_MODEL) && defined(LYFLOW_GAP_TEMPLATE)
  expected.insert("gap.locate_model_template_fallback");
#endif
  CHECK(actual == expected);
  const auto manifest = Json::parse(registry.toManifestJson());
  REQUIRE(manifest.contains("snippets"));
  std::set<std::string> exported;
  for (const auto& entry : manifest.at("snippets"))
    if (entry.at("id").get<std::string>().rfind("gap.", 0) == 0)
      exported.insert(entry.at("id"));
  CHECK(exported == expected);
}

TEST_CASE(
    "native flush snippets execute real scan and ROI dependencies with SI geometry and mm output") {
  ensureRegistry();
  Location location;
  for (const char* id : {"gap.flush_line_end", "gap.flush_selected_point"}) {
    CAPTURE(id);
    const auto graph = closed(snippet(id), location);
    test::Session session(graph);
    const auto& log = session.wait();
    for (const auto& event : log.ofKind("node_state"))
      if (event.value("state", "") == "error")
        MESSAGE(event.dump());
    REQUIRE(log.runStatus() == "ok");
    CHECK(log.seqIsDense());
    Data result;
    REQUIRE(exec::ResultStore::instance().get(session.runId(), "flush", "value", result));
    REQUIRE(result.asMeasurement());
    CHECK(result.asMeasurement()->ok);
    CHECK(result.asMeasurement()->unit == "mm");
    CHECK(result.asMeasurement()->value == doctest::Approx(-.5).epsilon(.01));
    REQUIRE(exec::ResultStore::instance().get(session.runId(), "judge", "value", result));
    CHECK(result.asMeasurement()->verdict == "ok");
    CHECK(result.asMeasurement()->hasLimits);
  }
}

TEST_CASE("gap judgement keeps inclusive limits margin patrol and failed measurement semantics") {
  const auto evaluate = [](double value, bool ok = true, double margin = 0, bool patrol = false) {
    Measurement measurement;
    measurement.value = value;
    measurement.ok = ok;
    measurement.unit = "mm";
    measurement.message = "upstream evidence";
    test::OpCall call;
    call.inputs["value"] = Data::measurement(measurement);
    REQUIRE(call.run("gap.judge", {{"nominal", Value::number(3.5)},
                                   {"upper", Value::number(1)},
                                   {"lower", Value::number(-1)},
                                   {"margin", Value::number(margin)},
                                   {"patrol", Value::boolean(patrol)}})
                .ok);
    const auto result = *call.out("value").asMeasurement();
    CHECK(result.unit == "mm");
    CHECK(result.message == measurement.message);
    CHECK(result.hasLimits);
    CHECK(result.nominal == 3.5);
    CHECK(result.lower == 2.5);
    CHECK(result.upper == 4.5);
    CHECK(measurement.verdict.empty());
    return result.verdict;
  };
  CHECK(evaluate(3.5) == "ok");
  CHECK(evaluate(2.5) == "ok");
  CHECK(evaluate(4.5) == "ok");
  CHECK(evaluate(4.6) == "high");
  CHECK(evaluate(2.4) == "low");
  CHECK(evaluate(4.45, true, .1) == "margin");
  CHECK(evaluate(2.55, true, .1) == "margin");
  CHECK(evaluate(4.6, true, 0, true) == "margin");
  CHECK(evaluate(2.4, true, 0, true) == "margin");
  CHECK(evaluate(3.5, false) == "fail");
  CHECK(evaluate(std::numeric_limits<double>::quiet_NaN()) == "fail");
  CHECK(evaluate(std::numeric_limits<double>::infinity(), true, 0, true) == "fail");
}

TEST_CASE(
    "native gap historical majors require explicit graph upgrade without unsafe automatic edits") {
  for (const auto& entry :
       {std::make_pair("gap.read_scan", "1.1.0"), std::make_pair("gap.result_bundle", "1.0.0"),
        std::make_pair("gap.result_bundle", "2.0.0")}) {
    CAPTURE(entry.first);
    CAPTURE(entry.second);
    auto graph = test::makeGraph({{"legacy", entry.first, Json::object()}}, {});
    graph["nodes"][0]["opVersion"] = entry.second;
    const auto original = graph.dump();
    exec::RawGraph raw;
    Diagnostics diagnostics;
    REQUIRE(exec::parseGraph(original, raw, diagnostics));
    exec::Plan plan;
    exec::BuildOptions options;
    options.runId = "gap-historical-major";
    REQUIRE(exec::buildPlan(ensureRegistry(), raw, options, plan, diagnostics));
    REQUIRE(plan.nodes.size() == 1);
    CHECK_FALSE(plan.nodes.front().valid);
    bool blocked = false;
    for (const auto& diagnostic : diagnostics.items()) {
      CHECK(diagnostic.migration == nullptr);
      if (diagnostic.status.message.find("graph_upgrade_required") != std::string::npos)
        blocked = true;
    }
    CHECK(blocked);
    CHECK(graph.dump() == original);
    CHECK(raw.nodes.front().opVersion == entry.second);
  }
}

#ifdef LYFLOW_GAP_IMPORT
TEST_CASE(
    "gap import capability closure rejects unavailable modes without publishing dangling graphs") {
  test::gap_graph_test::RuntimeFixture fixture;
  fixture.model();
  const auto& registry = ensureRegistry();
  for (const char* kind :
       {"StandardGap.yml", "StandardGap.yml:fine", "StandardGap.yml:template",
        "StandardGap.yml:template:fine", "StandardGap.yml:model", "StandardGap.yml:model:fine"}) {
    CAPTURE(kind);
    const bool model = std::string(kind).find(":model") != std::string::npos;
    const bool available = model ? registry.find("gap.infer_profile") != nullptr
                                 : registry.find("gap.align_template") != nullptr;
    const auto* importer = registry.findImporter(kind);
    REQUIRE(importer);
    std::string output = "not-published";
    const auto status = importer->fn(test::gap_graph_test::kRunConfig, fixture.root, output);
    if (!available) {
      CHECK_FALSE(status.ok);
      CHECK(bool(status.phase == Phase::Validate));
      CHECK(status.message.find("缺少") != std::string::npos);
      CHECK(output == "not-published");
    } else {
      REQUIRE_MESSAGE(status.ok, status.message);
      const auto graph = Json::parse(output);
      for (const auto& node : graph.at("nodes")) {
        const auto* op = registry.find(node.at("op").get<std::string>());
        REQUIRE(op);
        CHECK(node.at("opVersion") == op->version);
      }
    }
  }
}
#endif

#if defined(LYFLOW_GAP_TEMPLATE) && defined(LYFLOW_GAP_MODEL) && defined(LYFLOW_GAP_IMPORT)
namespace {
Json runtimeSnippet(const SnippetDesc& entry) {
  auto graph = test::makeGraph({}, {});
  graph["nodes"] = entry.body.at("nodes");
  graph["edges"] = entry.body.at("edges");
  std::set<std::string> sources;
  int edge = 0;
  for (auto& value : graph["edges"])
    value["id"] = "snippet-edge-" + std::to_string(edge++);
  for (const auto& input : entry.body.at("ports").at("inputs")) {
    const auto port = input.at("port").get<std::string>();
    const auto source = "actual_" + port;
    if (sources.insert(source).second)
      graph["nodes"].push_back({{"id", source},
                                {"op", "util.reroute"},
                                {"opVersion", ensureRegistry().find("util.reroute")->version},
                                {"params", Json::object()}});
    graph["edges"].push_back({{"id", "snippet-edge-" + std::to_string(edge++)},
                              {"from", {{"node", source}, {"port", "out"}}},
                              {"to", {{"node", input.at("node")}, {"port", port}}}});
  }
  return graph;
}
Json effectiveParams(const Json& graph, const std::string& id) {
  Json result = Json::object();
  for (const auto& node : graph.at("nodes"))
    if (node.at("id") == id)
      result = node.value("params", Json::object());
  if (graph.contains("params"))
    for (const auto& parameter : graph.at("params"))
      for (const auto& binding : parameter.at("binds")) {
        const auto path = binding.get<std::string>();
        const auto prefix = id + ".";
        if (path.rfind(prefix, 0) == 0)
          result[path.substr(prefix.size())] = parameter.at("default");
      }
  return result;
}
void snippetParams(Json& graph, const std::string& id, const Json& params) {
  for (auto& node : graph["nodes"])
    if (node.at("id") == id) {
      node["params"] = params;
      return;
    }
  FAIL(id);
}
void sameBoundBundle(const Data& actualData, const Data& expectedData) {
  const auto* actual = actualData.asBundle();
  const auto* expected = expectedData.asBundle();
  REQUIRE(actual);
  REQUIRE(expected);
  CHECK(actual->kind == expected->kind);
  REQUIRE(actual->fields.size() == expected->fields.size());
  std::set<std::string> names;
  for (const auto& field : actual->fields) {
    CAPTURE(field.first);
    CHECK(names.insert(field.first).second);
    const auto* value = expected->field(field.first);
    REQUIRE(value);
    CHECK(bool(field.second.kind() == value->kind()));
    if (field.second.asProfile())
      CHECK(field.second.profilePtr() == value->profilePtr());
    else
      CHECK(Json::parse(field.second.valueJson()) == Json::parse(value->valueJson()));
  }
}
void validSnippetMeasurement(const Data& value, double expected) {
  REQUIRE(value.asMeasurement());
  CHECK(value.asMeasurement()->ok);
  CHECK(value.asMeasurement()->unit == "mm");
  CHECK(value.asMeasurement()->value == doctest::Approx(expected).epsilon(.02));
}
}
TEST_CASE(
    "all six packaged native GAP snippets execute actual file model geometry and fallback dependencies") {
  using namespace test::gap_graph_test;
  RuntimeFixture fixture;
  const auto model = fixture.model(), missingLabels = fixture.model(true);
  const auto imported = fixture.graph("StandardGap.yml:template");
  test::Session source(imported, fixture.root, {}, false, 1, true);
  const auto& sourceLog = source.wait();
  requireRun(sourceLog);
  REQUIRE(sourceLog.runStatus() == "ok");
  const auto scan = result(source, "n_locate", "scan"), rois = result(source, "n_locate", "rois"),
             raw = result(source, "n_scan", "scan");
  const double expectedGap = result(source, "n_gap", "value").asMeasurement()->value;
  const double expectedFlush = result(source, "n_flush", "value").asMeasurement()->value;
  std::set<std::string> executed;
  for (const char* id : {"gap.flush_line_end", "gap.flush_selected_point", "gap.gap_circles",
                         "gap.measure_skeleton"}) {
    CAPTURE(id);
    auto graph = runtimeSnippet(snippet(id));
    if (std::string(id) == "gap.gap_circles" || std::string(id) == "gap.measure_skeleton")
      snippetParams(graph, "circles", effectiveParams(imported, "n_circles"));
    test::Session session(graph, fixture.root, {}, false, 1, true,
                          {{"actual_scan", "out", scan}, {"actual_rois", "out", rois}});
    const auto& log = session.wait();
    requireRun(log);
    REQUIRE(log.runStatus() == "ok");
    for (const auto& output : snippet(id).body.at("ports").at("outputs")) {
      const auto value = result(session, output.at("node").get<std::string>().c_str(),
                                output.at("port").get<std::string>().c_str());
      if (output.at("port") == "bundle") {
        REQUIRE(value.asRecord());
        CHECK(value.asRecord()->data.at("schemaVersion") == 4);
        CHECK(value.asRecord()->data.at("roi_source") == "config");
        CHECK(value.asRecord()->data.at("gap").at("status") == "success");
        CHECK(value.asRecord()->data.at("flush").at("status") == "success");
      } else {
        const bool gap = output.at("node") == "gap" || output.at("node") == "judgeGap" ||
                         std::string(id) == "gap.gap_circles";
        validSnippetMeasurement(value, gap ? expectedGap : expectedFlush);
      }
    }
    executed.insert(id);
  }

  const std::string yaml = std::string(kRunConfig) + "\nmodel_roi: {enabled: false, model_path: '" +
                           model.generic_u8string() + "'}\n";
  const auto modelGraph = fixture.graph("StandardGap.yml:model", yaml);
  for (const bool failModel : {false, true}) {
    CAPTURE(failModel);
    auto graph = runtimeSnippet(snippet("gap.locate_model_template_fallback"));
    auto modelParams = effectiveParams(modelGraph, "n_model");
    modelParams["modelPath"] = (failModel ? missingLabels : model).generic_u8string();
    snippetParams(graph, "model", modelParams);
    snippetParams(graph, "template", effectiveParams(imported, "n_locate"));
    test::Session session(graph, fixture.root, {}, false, 1, true, {{"actual_scan", "out", raw}});
    const auto& log = session.wait();
    requireRun(log);
    REQUIRE(log.runStatus() == "ok");
    const auto choice = result(session, "choice", "choice");
    REQUIRE(choice.asRecord());
    CHECK(choice.asRecord()->data.at("choice") == (failModel ? "b" : "a"));
    const auto selectedScan = result(session, "selected", "scan"),
               selectedRois = result(session, "selected", "rois");
    Data location;
    std::string why;
    REQUIRE_MESSAGE(scene::makeLocation(selectedScan, selectedRois, location, why), why);
    const char* producer = failModel ? "template" : "model";
    sameBoundBundle(selectedScan, result(session, producer, "scan"));
    sameBoundBundle(selectedRois, result(session, producer, "rois"));
    if (!failModel)
      for (const auto& event : log.ofKind("node_state"))
        if (event.value("nodeId", "") == "template" ||
            event.value("nodeId", "") == "templateLocation")
          CHECK(event.value("state", "") != "running");
  }
  executed.insert("gap.locate_model_template_fallback");

  for (const bool failPrimary : {false, true}) {
    CAPTURE(failPrimary);
    auto graph = runtimeSnippet(snippet("gap.backup_camera"));
    auto params = effectiveParams(imported, "n_scan");
    params["source"] = "dir";
    params["dir"] =
        (failPrimary ? fixture.root / "missing-primary" : fixture.root).generic_u8string();
    snippetParams(graph, "main", params);
    params["dir"] =
        (failPrimary ? fixture.root : fixture.root / "unused-backup").generic_u8string();
    snippetParams(graph, "backup", params);
    test::Session session(graph, fixture.root, {}, false, 1, true);
    const auto& log = session.wait();
    requireRun(log);
    REQUIRE(log.runStatus() == "ok");
    CHECK(result(session, "scan", "choice").asRecord()->data.at("choice") ==
          (failPrimary ? "b" : "a"));
    const auto selected = result(session, "scan", "out");
    scene::ScanView view;
    std::string why;
    REQUIRE(scene::readScanPair(selected, view, why));
    CHECK(view.sensorPrimary->asProfile()->pointCount() == 1280);
    CHECK(view.sensorPrimary->asProfile()->validity[7] == 0);
    CHECK(view.sensorPrimary->asProfile()->cloud->rgb[3 * 100] == 100);
    CHECK(selected.valueJson() ==
          result(session, failPrimary ? "backup" : "main", "scan").valueJson());
    if (!failPrimary)
      for (const auto& event : log.ofKind("node_state"))
        if (event.value("nodeId", "") == "backup")
          CHECK(event.value("state", "") != "running");
  }
  executed.insert("gap.backup_camera");
  REQUIRE(executed.size() == 6);
  for (const auto& entry : ensureRegistry().snippets())
    if (entry.id.rfind("gap.", 0) == 0)
      CHECK(executed.count(entry.id) == 1);
}
#endif
