#pragma once

#include <string>
#include <vector>

#include "gap_ml/RoiBoxes.hpp"
#include "lyflow/operator.h"
#include "profile_scene.h"

namespace lyflow::packs::gap::profile_model {
inline constexpr const char* kLabelRowsKind = profile_scene::kLabelRowsKind;
inline constexpr const char* kLabelInfoType = "gap.LabelRowsInfo.v2";

struct LabelRowsView {
  const Tensor* labels = nullptr;
  const Record* info = nullptr;
  int classCount = 0;
};

Status buildTensor(const Data& sensorPrimary, const Data& sensorSecondary, Tensor& result,
                   ExecContext& context);
Status makeLabelRows(const Tensor& labels, int classCount, const Data& sensorPrimary,
                     const Data& sensorSecondary, Data& result, ExecContext& context);
Status labelsFromLogits(const Tensor& logits, const Data& sensorPrimary,
                        const Data& sensorSecondary, Data& result, ExecContext& context,
                        const Tensor* modelInput = nullptr);
bool readLabelRows(const Data& data, LabelRowsView& result, std::string& why);
bool matchesSensors(const Data& labels, const Data& sensorPrimary, const Data& sensorSecondary,
                    std::string& why);

struct BoxComputation {
  bool complete = false;
  profile_scene::RoiFields fields;
  Data seam;
  Data seamContext;
  Record refinements;
  std::vector<std::string> missingSegments;
};

Status boxesFromLabels(const Data& labels, const Data& scan,
                       const ::gap::ml::MaskRefineOptions& options, BoxComputation& result,
                       ExecContext& context);
} // namespace lyflow::packs::gap::profile_model
