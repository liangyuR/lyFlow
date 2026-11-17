#pragma once

#include "profile_scene.h"
#include "lyflow/operator.h"

namespace lyflow::packs::gap::profile_scene {
struct OverallRoiOptions {
  std::array<float, 4> roiMm{-1, -1, 1, 1};
  std::string mode = "fixed";
  std::string usingCamera = "Both";
};
struct DatumWindowOptions {
  std::string side = "left";
  double startMm = 0;
  double lengthMm = 13;
  double heightMm = 2.5;
};
struct GeometryResult {
  Data box;
  Record context;
  Record status;
};
struct RollCropOptions {
  bool enabled = true;
  double halfWidth = 35;
  double halfHeight = 20;
  double maxRollBoxHeight = 8;
  std::int64_t minPointsKept = 50;
  std::string usingCamera = "Both";
};
struct RollCropResult {
  Data scan;
  Data window;
  Record windowContext;
  Record status;
};
Status overallRoi(const Data& primary, const Data& secondary, const OverallRoiOptions& options,
                  GeometryResult& result, ExecContext& context);
Status datumWindow(const Data& anchor, const Record& anchorContext, const Data* heightAnchor,
                   const Record* heightContext, const DatumWindowOptions& options,
                   GeometryResult& result, ExecContext& context);
Status rollAnchoredCrop(const Data& scan, const Data& gapLeft, const Record& gapLeftContext,
                        const Data& gapRight, const Record& gapRightContext,
                        const RollCropOptions& options, RollCropResult& result,
                        ExecContext& context);
void registerProfileRegionOps(Registry& registry);
} // namespace lyflow::packs::gap::profile_scene
