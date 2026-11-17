#pragma once

#include <cmath>
#include <limits>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow_profile/provenance.h"

namespace lyflow::profile_contract {
struct SelectionOptions {
  std::string mode = "compact";
  std::string emptyPolicy = "error";
  std::string bounds = "closed";
  std::string invalidPolicy = "reject";
};
struct SelectionResult {
  Data profile;
  Indices selected;
  Indices rejected;
  Record provenance;
  Record selection;
};
namespace selection_detail {
using Json = nlohmann::json;
inline Status bad(const std::string& message, const char* port = "profile",
                  const char* code = "bad_input") {
  return Status::Error(Phase::Execute, code, message, {}, port);
}
inline Status cancelled() { return bad("轮廓选区已取消", "profile", "cancelled"); }
inline Status select(const Data& source, const Data* boxData, const Record* boxContext,
                     const Indices* indices, const Record* provenance,
                     const SelectionOptions& options, SelectionResult& outputResult,
                     ExecContext& ctx, bool byBox) {
  const auto* input = source.asProfile();
  std::string why;
  if (ctx.cancelled())
    return cancelled();
  const auto n = input->pointCount();
  if (n > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    return bad("轮廓槽数超过Indices范围", "profile", "budget_exceeded");
  const auto& mode = options.mode;
  const auto& empty = options.emptyPolicy;
  Json sources;
  if (!profile_contract::profileSources(source, provenance, sources, why))
    return bad(why, "provenance");
  Json references = Json::array();
  std::vector<std::uint8_t> accepted(n, 0);
  if (byBox) {
    const auto* box = boxData ? boxData->asBox2D() : nullptr;
    ProfileDescriptor space;
    Json boxSources;
    if (!box ||
        !profile_contract::geometryMatchesPhysical(*boxData, boxContext, space, why, &boxSources))
      return bad(why.empty() ? "需要米制Box2D和有效上下文" : why, "boxContext");
    references = boxSources;
    if (!profile_contract::sameSpace(input->descriptor, space))
      return bad("ROI与轮廓不在相同物理空间", "boxContext");
    const auto& bounds = options.bounds;
    Profile metric;
    if (!convertProfileUnit(*input, "m", metric, why, [&ctx] { return ctx.cancelled(); }))
      return ctx.cancelled() ? cancelled() : bad(why);
    const int a = input->descriptor.plane == "YZ" ? 1 : 0;
    const int b = input->descriptor.plane == "XY" ? 1 : 2;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      if (!input->validity[i])
        continue;
      const float x = metric.cloud->xyz[3 * i + a], y = metric.cloud->xyz[3 * i + b];
      accepted[i] =
          bounds == "open"
              ? x > box->min[0] && x < box->max[0] && y > box->min[1] && y < box->max[1]
              : x >= box->min[0] && x <= box->max[0] && y >= box->min[1] && y <= box->max[1];
    }
  } else {
    if (!indices || indices->sourceCloudId != input->cloud->id)
      return bad("Indices必须绑定当前Profile实际cloud.id", "indices");
    const auto& invalid = options.invalidPolicy;
    std::int32_t previous = -1;
    for (std::size_t i = 0; i < indices->values.size(); ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      const auto slot = indices->values[i];
      if (slot < 0 || static_cast<std::size_t>(slot) >= n || slot <= previous)
        return bad("Indices必须为范围内严格升序唯一的原输入槽", "indices");
      if (!input->validity[slot] && invalid == "reject")
        return bad("选择包含无效槽；保留原无效载荷须显式invalidPolicy=preserve", "indices");
      accepted[slot] = 1;
      previous = slot;
    }
  }
  Indices selected{input->cloud->id, {}}, rejected{input->cloud->id, {}};
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 1023U) == 0 && ctx.cancelled())
      return cancelled();
    (accepted[i] ? selected.values : rejected.values).push_back(static_cast<std::int32_t>(i));
  }
  if (selected.values.empty() && empty == "error")
    return bad("选区没有样本", "profile", "no_result");
  Profile result = *input;
  if (mode == "compact") {
    auto cloud = input->cloud->select(selected.values);
    result.cloud = std::make_shared<const PointCloud>(std::move(cloud));
    result.validity.clear();
    result.sourceIndices.clear();
    for (std::size_t i = 0; i < selected.values.size(); ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      const auto slot = static_cast<std::size_t>(selected.values[i]);
      result.validity.push_back(input->validity[slot]);
      result.sourceIndices.push_back(input->sourceIndices[slot]);
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && ctx.cancelled())
        return cancelled();
      result.validity[i] = input->validity[i] && accepted[i];
    }
  }
  const auto data = Data::profile(std::move(result));
  Record rebound;
  if (!profile_contract::rebindProvenance(source, data, provenance, rebound, why))
    return bad(why, "provenance");
  if (!references.empty()) {
    Record extended;
    if (!profile_contract::extendProvenanceSources(data, rebound, references, extended, why))
      return bad(why, "boxContext");
    rebound = std::move(extended);
  }
  const auto selection = Data::record(Record{"profile.RegionSelection.v1",
                                             {{"schemaVersion", 1},
                                              {"inputContext", profile_contract::context(source)},
                                              {"outputContext", profile_contract::context(data)},
                                              {"mode", mode},
                                              {"indexSpace", "input_profile_slots"},
                                              {"sourceCloudId", std::to_string(input->cloud->id)},
                                              {"selectedCount", selected.values.size()},
                                              {"rejectedCount", rejected.values.size()},
                                              {"sourceIndexMapping", "profile.sourceIndices"}}});
  if (ctx.cancelled())
    return cancelled();
  SelectionResult candidate;
  candidate.profile = data;
  candidate.selected = std::move(selected);
  candidate.rejected = std::move(rejected);
  candidate.provenance = std::move(rebound);
  candidate.selection = *selection.asRecord();
  outputResult = std::move(candidate);
  return Status::Ok();
}
} // namespace selection_detail
inline Status cropProfile(const Data& source, const Data& box, const Record& boxContext,
                          const Record* provenance, const SelectionOptions& options,
                          SelectionResult& result, ExecContext& ctx) {
  return selection_detail::select(source, &box, &boxContext, nullptr, provenance, options, result,
                                  ctx, true);
}
inline Status selectProfileIndices(const Data& source, const Indices& indices,
                                   const Record* provenance, const SelectionOptions& options,
                                   SelectionResult& result, ExecContext& ctx) {
  return selection_detail::select(source, nullptr, nullptr, &indices, provenance, options, result,
                                  ctx, false);
}
} // namespace lyflow::profile_contract
