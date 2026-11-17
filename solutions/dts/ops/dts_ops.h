#pragma once
#include <nlohmann/json.hpp>

#include "algo/profile.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow_profile/context.h"

namespace lyflow::dts {

struct ActiveProfile {
  Profile samples;
  std::vector<std::size_t> slots;
  const lyflow::Profile* native = nullptr;
};

Status loadProfile(const Data& source, ActiveProfile& output, ExecContext& ctx);
Status sceneError(const std::exception& error, const char* port);
Status cancelledStatus();
nlohmann::json requireGraphMigration(const nlohmann::json& params);
const nlohmann::json& checkedRecord(const Data& data, const char* type);
nlohmann::json sceneRecord();
std::vector<Piece> checkedPieces(const Data& data, const ActiveProfile& active);
double number(const nlohmann::json& data, const char* field);
Line2D metricGeometry(const Line2D& legacy);
Point2D metricGeometry(const Point2D& legacy);
nlohmann::json lineMillimeters(const Line2D& line);
Line2D lineMillimeters(const nlohmann::json& data);

std::vector<Piece> piecesFromJson(const nlohmann::json& faces);
std::vector<Face> facesFromJson(const nlohmann::json& faces);
nlohmann::json faceToJson(const Face& f);

const nlohmann::json* recordData(const Data& d, const char* type);

void registerProfileIn(Registry& r);
void registerGraphMigrations(Registry& r);
void registerSealDome(Registry& r);
void registerPickMetal(Registry& r);
void registerSealRoot(Registry& r);
void registerFlush(Registry& r);
void registerProfileBundle(Registry& r);
void registerGeometryAdapter(Registry& r);

} // namespace lyflow::dts
