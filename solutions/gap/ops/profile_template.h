#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "lyflow/operator.h"
#include "profile_scene.h"

namespace lyflow::packs::gap::profile_template {
inline constexpr const char* kTemplatePairKind = "gap.TemplatePair.v2";
inline constexpr const char* kTemplateInfoType = "gap.TemplateInfo.v2";
inline constexpr const char* kAlignmentKind = "gap.Alignment.v2";
inline constexpr const char* kAlignmentInfoType = "gap.AlignmentInfo.v2";

struct TemplateLoadOptions {
  std::filesystem::path dir;
  std::string left = "f1_left.pcd", right = "f1_right.pcd";
  std::string layout = "profile", unit = "m";
  std::string fileFrame = "template-file", templateFrame = "template-frame";
  std::string handedness = "reflected";
  std::string leftSourceId, rightSourceId;
};
struct AlignmentOptions {
  std::string templateId = "primary", initialPoseMode = "endpoint_midpoint", roisFrom = "template";
  std::int64_t order = 0;
  double maxMatchingDist = 10, maxFitnessDist = .5, trustTranslation = 3, trustRotation = 2;
  double degenerateRatio = 1e-3;
  int maxIterations = 1000, normalKnn = 10, minScore = 60;
  bool bidirection = false, globalCoarse = true, successGuide = false, segRoi = true;
  std::array<std::array<float, 4>, 4> roisMm{};
};
struct TemplateView {
  const Data* left = nullptr;
  const Data* right = nullptr;
  const Record* info = nullptr;
};
struct AlignmentView {
  const Data* target = nullptr;
  const Record* targetProvenance = nullptr;
  const Data* templateLeft = nullptr;
  const Data* templateRight = nullptr;
  const Data* alignedLeft = nullptr;
  const Data* alignedRight = nullptr;
  const Record* info = nullptr;
};

AlignmentOptions alignmentOptions(const ParamView& parameters);
TemplateLoadOptions loadOptions(const ParamView& parameters);
std::vector<Param> alignmentParameters();
std::vector<Param> loadParameters();
std::vector<Param> selectionParameters();
std::vector<Param> businessRoiParameters();
std::string templateFingerprint(const TemplateLoadOptions& options);

bool makeTemplatePair(const Data& left, const Data& right, const nlohmann::json& origins,
                      Data& result, std::string& why);
bool readTemplatePair(const Data& data, TemplateView& result, std::string& why);
Status loadTemplatePair(const TemplateLoadOptions& options, Data& result, ExecContext& context);
Status alignClouds(const PointCloud& target, const PointCloud& left, const PointCloud& right,
                   const AlignmentOptions& options, Record& result, ExecContext& context);
bool readAlignment(const Data& data, AlignmentView& result, std::string& why);
Status align(const Data& scan, const Data& templates, const AlignmentOptions& options, Data& result,
             ExecContext& context);
Status select(const std::vector<Data>& candidates, int minScore, Data& result,
              ExecContext& context);
Status transformBusinessRois(const Record& alignment, const std::string& datumSide,
                             std::array<Box2D, 4>& boxes, Box2D& seam, std::string& resolvedSide);
Status businessRois(const Data& alignment, const std::string& datumSide,
                    profile_scene::RoiFields& result, Data& seam, Data& seamContext,
                    ExecContext& context);
void registerProfileTemplateOps(Registry& registry);
} // namespace lyflow::packs::gap::profile_template
