#pragma once
#include <nlohmann/json.hpp>

#include "algo/profile.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::dts {

Profile profileFromCloud(const PointCloud& cloud);
PointCloud cloudFromProfile(const Profile& p, const std::vector<float>& intensity);

std::vector<Piece> piecesFromJson(const nlohmann::json& faces);
std::vector<Face> facesFromJson(const nlohmann::json& faces);
nlohmann::json faceToJson(const Face& f);

const nlohmann::json* recordData(const Data& d, const char* type);

void registerProfileIn(Registry& r);
void registerProfileClean(Registry& r);
void registerSplitFaces(Registry& r);
void registerSealDome(Registry& r);
void registerPickMetal(Registry& r);
void registerSealRoot(Registry& r);
void registerFlush(Registry& r);
void registerProfileBundle(Registry& r);

}  // namespace lyflow::dts
