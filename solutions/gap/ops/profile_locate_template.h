#pragma once

#include <array>
#include <set>

#include "profile_regions.h"
#include "profile_template.h"

namespace lyflow::packs::gap::profile_template {
struct TemplateSlot {
  bool enabled = false;
  TemplateLoadOptions load;
  AlignmentOptions alignment;
};
struct LocateOptions {
  profile_scene::OverallRoiOptions overall{{-1000, -1000, 1000, 1000}, "fixed", "Both"};
  std::array<TemplateSlot, 4> slots;
  int minScore = 60;
};
struct LocateResult {
  Data scan;
  Data rois;
  Data alignment;
};
LocateOptions locateOptions(const ParamView& parameters);
std::vector<Param> locateParameters();
std::vector<Issue> validateLocateOptions(const LocateOptions& options);
std::string locateFingerprint(const LocateOptions& options);
Status locate(const Data& scan, const LocateOptions& options, LocateResult& result,
              ExecContext& context);
void registerProfileLocateTemplate(Registry& registry);
} // namespace lyflow::packs::gap::profile_template
