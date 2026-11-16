#include <doctest/doctest.h>

#include <limits>
#include <vector>

#include "lyflow_profile/context.h"

using namespace lyflow;

namespace {
Data source(const char* id, const char* unit = "m") {
  PointCloud cloud;
  cloud.push(0.01F, 0.02F, 0);
  cloud.push(0.03F, 0.04F, 0);
  Profile profile;
  std::string why;
  REQUIRE(makeProfile(std::make_shared<const PointCloud>(cloud),
                      {1, "measurement", unit, "XY", "right", "acquisition", id, 8}, {1, 1}, {1, 7},
                      profile, why));
  return Data::profile(std::move(profile));
}
std::vector<Data> shapes() {
  Point2D point;
  point.p[0] = 0.01F;
  point.p[1] = 0.02F;
  Line2D line;
  line.point[0] = 0.01F;
  line.point[1] = 0.02F;
  line.dir[0] = 1;
  line.dir[1] = 0;
  Circle2D circle;
  circle.center[0] = 0.01F;
  circle.center[1] = 0.02F;
  circle.radius = 0.005F;
  Box2D box;
  box.min[0] = 0;
  box.min[1] = 0;
  box.max[0] = 0.1F;
  box.max[1] = 0.1F;
  return {Data::point2d(point), Data::line2d(line), Data::circle2d(circle), Data::box2d(box)};
}
}

TEST_CASE("Physical geometry context binds four metric shapes and retains the v1 contract") {
  const auto profile = source("scan-A");
  const auto geometry = shapes();
  for (const auto& shape : geometry) {
    Record record{"profile.Geometry.v2", profile_contract::geometryContextV2(profile, shape)};
    ProfileDescriptor descriptor;
    std::string why;
    nlohmann::json sources;
    REQUIRE(profile_contract::geometryMatchesPhysical(shape, &record, descriptor, why, &sources));
    CHECK(descriptor.sourceId == "scan-A");
    CHECK(descriptor.frame == "measurement");
    CHECK(sources == nlohmann::json::array({profile_contract::context(profile)}));
    CHECK(record.data["geometryType"] == shape.typeName());
    CHECK(record.data["geometryUnit"] == "m");
    CHECK(record.data["relation"] == "derived");
  }
  for (std::size_t i = 0; i < 2; ++i) {
    Record legacy{"profile.Geometry.v1", profile_contract::geometryContext(profile, geometry[i])};
    ProfileDescriptor descriptor;
    std::string why;
    REQUIRE(profile_contract::geometryMatchesPhysical(geometry[i], &legacy, descriptor, why));
    CHECK(legacy.data["schemaVersion"] == 1);
  }
  CHECK_THROWS_AS(profile_contract::geometryContext(profile, geometry[2]), std::invalid_argument);
  auto pixel = *geometry[0].asPoint2D();
  pixel.unit = Unit2D::Pixel;
  CHECK_THROWS_AS(profile_contract::geometryContextV2(profile, Data::point2d(pixel)),
                  std::invalid_argument);
}

TEST_CASE("Derived geometry retains all physical sources and checks spatial compatibility") {
  const auto a = source("scan-A", "mm"), b = source("scan-B");
  const auto contextA = profile_contract::context(a), contextB = profile_contract::context(b);
  const auto shape = shapes()[2];
  Record record{"profile.Geometry.v2",
                profile_contract::geometryContextV2FromSources(
                    shape, nlohmann::json::array({contextA, contextB, contextA}))};
  ProfileDescriptor descriptor;
  std::string why;
  nlohmann::json sources;
  REQUIRE(profile_contract::geometryMatchesPhysical(shape, &record, descriptor, why, &sources));
  CHECK(sources == nlohmann::json::array({contextA, contextB}));
  CHECK(descriptor.unit == "mm");
  const auto original = record;
  for (const char* field : {"frame", "plane", "handedness"}) {
    auto invalid = contextB;
    invalid["descriptor"][field] = std::string(field) == "plane"        ? "XZ"
                                   : std::string(field) == "handedness" ? "reflected"
                                                                        : "other-frame";
    CHECK_THROWS_AS(profile_contract::geometryContextV2FromSources(
                        shape, nlohmann::json::array({contextA, invalid})),
                    std::invalid_argument);
  }
  auto changed = *shape.asCircle2D();
  changed.radius *= 2;
  descriptor.frame = "sentinel";
  sources = "sentinel";
  CHECK_FALSE(profile_contract::geometryMatchesPhysical(Data::circle2d(changed), &record,
                                                        descriptor, why, &sources));
  CHECK(descriptor.frame == "sentinel");
  CHECK(sources == "sentinel");
  CHECK(record.data == original.data);
}

TEST_CASE("Geometry contexts reject malformed provenance and preserve zero-length measurements") {
  const auto profile = source("scan-A");
  const auto shape = shapes()[1];
  const Record original{"profile.Geometry.v2", profile_contract::geometryContextV2(profile, shape)};
  ProfileDescriptor descriptor;
  std::string why;
  for (const char* mutation : {"version", "type", "extra", "duplicate", "relation", "empty"}) {
    auto record = original;
    const std::string which = mutation;
    if (which == "version")
      record.data["schemaVersion"] = 3;
    if (which == "type")
      record.data["geometryType"] = "Circle2D";
    if (which == "extra")
      record.data["unreviewed"] = true;
    if (which == "duplicate")
      record.data["sources"].push_back(record.data["sources"][0]);
    if (which == "relation")
      record.data["relation"] = "observed-by-guess";
    if (which == "empty")
      record.data["sources"] = nlohmann::json::array();
    CHECK_FALSE(profile_contract::geometryMatchesPhysical(shape, &record, descriptor, why));
  }
  auto line = *shape.asLine2D();
  line.hasSegment = true;
  line.start[0] = line.end[0] = 0.01F;
  line.start[1] = line.end[1] = 0.02F;
  const auto segment = Data::line2d(line);
  Record zero{"profile.Geometry.v2", profile_contract::geometryContextV2(profile, segment)};
  CHECK(profile_contract::geometryMatchesPhysical(segment, &zero, descriptor, why));
  CHECK_FALSE(profile_contract::validLine(segment, why));
  line.dir[0] = line.dir[1] = 0;
  CHECK_THROWS_AS(profile_contract::geometryContextV2(profile, Data::line2d(line)),
                  std::invalid_argument);
  auto circle = *shapes()[2].asCircle2D();
  circle.radius = std::numeric_limits<float>::infinity();
  CHECK_THROWS_AS(profile_contract::geometryContextV2(profile, Data::circle2d(circle)),
                  std::invalid_argument);
  nlohmann::json many = nlohmann::json::array();
  for (int i = 0; i < 65; ++i) {
    const auto id = "scan-" + std::to_string(i);
    many.push_back(profile_contract::context(source(id.c_str())));
  }
  CHECK_THROWS_AS(profile_contract::geometryContextV2FromSources(shape, many),
                  std::invalid_argument);
}
