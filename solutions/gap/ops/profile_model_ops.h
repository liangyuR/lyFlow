#pragma once

#include "profile_model.h"
#include "profile_regions.h"

namespace lyflow::packs::gap::profile_model {
struct ModelRegionResult {
  BoxComputation boxes;
  Data rois;
};
Status extractModelRegions(const Data& labels, const Data& scan,
                           const ::gap::ml::MaskRefineOptions& options, ModelRegionResult& result,
                           ExecContext& context);
Status labelsToCloud(const Data& scan, const Data& labels, const std::string& row, Data& result,
                     ExecContext& context);
struct LocateModelOptions {
  std::filesystem::path modelPath;
  ::gap::ml::MaskRefineOptions refine;
  profile_scene::RollCropOptions crop;
};
struct LocateModelResult {
  Data scan;
  Data rois;
  Data labels;
  Record refinements;
};
#ifdef LYFLOW_GAP_MODEL
Status inferProfileTensor(const Tensor& input, const std::filesystem::path& modelPath,
                          Tensor& result, ExecContext& context);
Status locateModel(const Data& scan, const LocateModelOptions& options, LocateModelResult& result,
                   ExecContext& context);
#endif
void registerProfileModelOps(Registry& registry);
} // namespace lyflow::packs::gap::profile_model
