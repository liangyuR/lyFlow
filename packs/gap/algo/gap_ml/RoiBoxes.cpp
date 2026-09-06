#include "gap_ml/RoiBoxes.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gap::ml {

namespace {

struct SegmentSpec {
  const char* name;
  int class_id;
};

// Class ids and segment order from ml_handoff/annotation/corrections.py SEGMENT_NAMES /
// SEGMENT_CLASS_ID -- fixed by the contract, not configurable here.
constexpr SegmentSpec kSegments[] = {
    {"left_surface", 2},
    {"left_roll", 3},
    {"right_roll", 5},
    {"right_surface", 6},
};

constexpr std::size_t kMinSegmentSlots = 3;
constexpr double kGapFactor = 10.0;
constexpr double kXScaleFloorMm = 0.4;
constexpr double kZScaleFloorMm = 1.2;
constexpr std::size_t kRobustMinPoints = 8;

// np.median on an already-sorted array: the middle element for an odd count, the average of the
// two middle elements for an even count. Deliberately distinct from robustClusterBounds' own
// "lower median" (s[(n-1)//2]) -- see onnx_port_contract.md section 3.
double medianOfSorted(const std::vector<double>& sorted) {
  const std::size_t n = sorted.size();
  if (n % 2 == 1) return sorted[n / 2];
  return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

void appendMasked(const SegmentRow& row, int class_id, std::vector<double>& xs,
                  std::vector<double>& zs) {
  const auto& x = *row.x_mm;
  const auto& z = *row.z_mm;
  const auto& valid = *row.valid;
  const auto& labels = *row.labels;
  const std::size_t n = x.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (valid[i] && labels[i] == class_id) {
      xs.push_back(x[i]);
      zs.push_back(z[i]);
    }
  }
}

// --- refine-v1 helpers -------------------------------------------------------------------------

struct Box {
  double x_lo = 0.0;
  double z_lo = 0.0;
  double x_hi = 0.0;
  double z_hi = 0.0;
};

using Component = std::vector<std::size_t>;

Box boxOf(const SegmentRow& row, const Component& slots) {
  const auto& x = *row.x_mm;
  const auto& z = *row.z_mm;
  Box box{x[slots.front()], z[slots.front()], x[slots.front()], z[slots.front()]};
  for (const std::size_t slot : slots) {
    box.x_lo = std::min(box.x_lo, x[slot]);
    box.x_hi = std::max(box.x_hi, x[slot]);
    box.z_lo = std::min(box.z_lo, z[slot]);
    box.z_hi = std::max(box.z_hi, z[slot]);
  }
  return box;
}

Box unite(const Box& a, const Box& b) {
  return Box{std::min(a.x_lo, b.x_lo), std::min(a.z_lo, b.z_lo), std::max(a.x_hi, b.x_hi),
             std::max(a.z_hi, b.z_hi)};
}

// Separation between two axis-aligned boxes; 0 when they touch or overlap.
double boxDistance(const Box& a, const Box& b) {
  const double dx = std::max(0.0, std::max(a.x_lo - b.x_hi, b.x_lo - a.x_hi));
  const double dz = std::max(0.0, std::max(a.z_lo - b.z_hi, b.z_lo - a.z_hi));
  return std::hypot(dx, dz);
}

// Slots labelled `class_id` in `row`, grouped into spatially connected components: an O(n) split
// into runs along the profile followed by a single-linkage merge of runs whose boxes are within
// options.link_mm. The merge is what lets an outlier that interleaves in *slot* order (a ghost
// return landing between two stretches of one real surface) split the run without splitting the
// feature. Components come back in ascending first-slot order, each with ascending slot indices.
std::vector<Component> componentsFor(const SegmentRow& row, int class_id,
                                     const MaskRefineOptions& options) {
  const auto& x = *row.x_mm;
  const auto& z = *row.z_mm;
  const auto& valid = *row.valid;
  const auto& labels = *row.labels;

  std::vector<Component> runs;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (!valid[i] || labels[i] != class_id) continue;
    if (!runs.empty()) {
      const std::size_t previous = runs.back().back();
      const bool slot_break = i - previous > options.split_slot_gap;
      const bool step_break =
          std::hypot(x[i] - x[previous], z[i] - z[previous]) > options.split_step_mm;
      if (!slot_break && !step_break) {
        runs.back().push_back(i);
        continue;
      }
    }
    runs.push_back(Component{i});
  }
  if (runs.size() <= 1) return runs;

  std::vector<Box> boxes;
  boxes.reserve(runs.size());
  for (const auto& run : runs) boxes.push_back(boxOf(row, run));

  std::vector<std::size_t> parent(runs.size());
  std::iota(parent.begin(), parent.end(), std::size_t{0});
  const std::function<std::size_t(std::size_t)> find = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  for (std::size_t i = 0; i < runs.size(); ++i) {
    for (std::size_t j = i + 1; j < runs.size(); ++j) {
      if (boxDistance(boxes[i], boxes[j]) > options.link_mm) continue;
      const std::size_t a = find(i);
      const std::size_t b = find(j);
      if (a != b) parent[a] = b;
    }
  }

  // First-appearance order keeps the output deterministic, and each component stays ascending
  // because run k's slots all precede run k+1's.
  std::vector<std::size_t> group_of_root(runs.size(), std::numeric_limits<std::size_t>::max());
  std::vector<Component> components;
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const std::size_t root = find(i);
    if (group_of_root[root] == std::numeric_limits<std::size_t>::max()) {
      group_of_root[root] = components.size();
      components.emplace_back();
    }
    auto& target = components[group_of_root[root]];
    target.insert(target.end(), runs[i].begin(), runs[i].end());
  }
  return components;
}

// Largest component with at least min_slots points; falls back to the largest component overall
// when none clears the bar (a segment made entirely of short pieces is still better than nothing
// -- boxesFromLabels' own MIN_SEGMENT_SLOTS check decides whether it ends up usable). Ties keep
// the earliest component.
const Component* pickLargest(const std::vector<Component>& components, std::size_t min_slots) {
  const Component* best = nullptr;
  for (const auto& component : components) {
    if (component.size() < min_slots) continue;
    if (best == nullptr || component.size() > best->size()) best = &component;
  }
  if (best != nullptr) return best;
  for (const auto& component : components) {
    if (best == nullptr || component.size() > best->size()) best = &component;
  }
  return best;
}

std::size_t labelledSlotCount(const SegmentRow& row, int class_id) {
  const auto& valid = *row.valid;
  const auto& labels = *row.labels;
  std::size_t count = 0;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (valid[i] && labels[i] == class_id) ++count;
  }
  return count;
}

detection::domain::Roi& fieldFor(detection::domain::TemplateRoiConfiguration& rois,
                                 const std::string& name) {
  if (name == "left_surface") return rois.flush_base;
  if (name == "left_roll") return rois.gap_left;
  if (name == "right_roll") return rois.gap_right;
  if (name == "right_surface") return rois.flush_ref;
  throw std::runtime_error("gap_ml::boxesFromLabels: unknown segment name " + name);
}

}  // namespace

ClusterBounds robustClusterBounds(std::vector<double> values, double gap_factor, double scale_floor,
                                  std::size_t min_points) {
  if (values.empty()) {
    throw std::runtime_error("gap_ml::robustClusterBounds requires at least one value");
  }
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  if (n < min_points) {
    return ClusterBounds{values.front(), values.back(), 0};
  }

  // Lower median: s[(n-1)//2], not the averaged-pair median used for the MAD scale below.
  const std::size_t mid = (n - 1) / 2;
  const double med = values[mid];

  std::vector<double> absolute_deviation(n);
  for (std::size_t i = 0; i < n; ++i) absolute_deviation[i] = std::fabs(values[i] - med);
  std::sort(absolute_deviation.begin(), absolute_deviation.end());
  const double mad = medianOfSorted(absolute_deviation);
  const double scale = std::max(mad * 1.4826, scale_floor);
  const double threshold = gap_factor * scale;

  std::size_t lo = 0;
  for (std::size_t idx = mid; idx > 0;) {
    --idx;
    if (values[idx + 1] - values[idx] > threshold) {
      lo = idx + 1;
      break;
    }
  }
  std::size_t hi = n - 1;
  for (std::size_t idx = mid; idx + 1 < n; ++idx) {
    if (values[idx + 1] - values[idx] > threshold) {
      hi = idx;
      break;
    }
  }
  const std::size_t trimmed = lo + (n - 1 - hi);
  return ClusterBounds{values[lo], values[hi], trimmed};
}

RoiBoxesResult boxesFromLabels(const SegmentRow& row0, const SegmentRow& row1) {
  RoiBoxesResult result;
  for (const auto& segment : kSegments) {
    std::vector<double> xs;
    std::vector<double> zs;
    appendMasked(row0, segment.class_id, xs, zs);
    appendMasked(row1, segment.class_id, xs, zs);
    if (xs.size() < kMinSegmentSlots) {
      result.missing_segments.emplace_back(segment.name);
      continue;
    }
    const auto x_bounds = robustClusterBounds(xs, kGapFactor, kXScaleFloorMm, kRobustMinPoints);
    const auto z_bounds = robustClusterBounds(zs, kGapFactor, kZScaleFloorMm, kRobustMinPoints);
    auto& roi = fieldFor(result.rois, segment.name);
    roi.values = {x_bounds.lo, z_bounds.lo, x_bounds.hi, z_bounds.hi};
  }
  return result;
}

MaskRefineResult refineSegmentMasks(const SegmentRow& row0, const SegmentRow& row1,
                                    const MaskRefineOptions& options) {
  MaskRefineResult result;
  result.labels[0] = *row0.labels;
  result.labels[1] = *row1.labels;
  if (!options.enabled) return result;

  constexpr std::size_t kRows = 2;
  constexpr std::size_t kSegmentCount = 4;
  // Indices into kSegments: {left_surface, left_roll, right_roll, right_surface}.
  constexpr std::size_t kLeftSurface = 0;
  constexpr std::size_t kLeftRoll = 1;
  constexpr std::size_t kRightRoll = 2;
  constexpr std::size_t kRightSurface = 3;
  const double kNoAnchor = std::numeric_limits<double>::quiet_NaN();

  const SegmentRow* rows[kRows] = {&row0, &row1};
  std::array<std::array<std::vector<Component>, kSegmentCount>, kRows> components;
  for (std::size_t r = 0; r < kRows; ++r) {
    for (std::size_t s = 0; s < kSegmentCount; ++s) {
      components[r][s] = componentsFor(*rows[r], kSegments[s].class_id, options);
    }
  }

  std::array<std::array<Component, kSegmentCount>, kRows> chosen;
  const auto record = [&](std::size_t r, std::size_t s, double anchor_distance, bool gate_missed,
                          double pair_distance = std::numeric_limits<double>::quiet_NaN(),
                          bool pairing_degraded = false) {
    const std::size_t labelled = labelledSlotCount(*rows[r], kSegments[s].class_id);
    const std::size_t kept = chosen[r][s].size();
    // A suspicion flag always gets an entry, even for the otherwise-silent single-component case:
    // the whole point of the flag is to surface a decision that could not be cross-checked.
    if (!pairing_degraded && components[r][s].size() <= 1 && kept == labelled) return;
    SegmentRefinement entry;
    entry.segment = kSegments[s].name;
    entry.row = r;
    entry.component_count = components[r][s].size();
    entry.kept_slots = kept;
    entry.dropped_slots = labelled - kept;
    entry.anchor_distance_mm = anchor_distance;
    entry.anchor_gate_missed = gate_missed;
    entry.pair_distance_mm = pair_distance;
    entry.pairing_degraded = pairing_degraded;
    result.refinements.push_back(std::move(entry));
  };

  // Candidates surviving the size floor, in componentsFor order: the components at or above
  // min_component_slots when any of them clears it, otherwise all of them. Same floor semantics as
  // pickLargest, hoisted out so the cross-row pairing below can enumerate them.
  const auto candidatesFor = [&](std::size_t r, std::size_t s) {
    std::vector<const Component*> candidates;
    bool any_above_floor = false;
    for (const auto& component : components[r][s]) {
      if (component.size() >= options.min_component_slots) any_above_floor = true;
    }
    for (const auto& component : components[r][s]) {
      if (any_above_floor && component.size() < options.min_component_slots) continue;
      candidates.push_back(&component);
    }
    return candidates;
  };

  // Pass 1 -- rolls. They carry no anchor of their own, but the two camera rows are calibrated
  // into one sensor frame, so the real roll must show up in nearly the same place in both rows.
  // Pair the rows' candidates and keep the closest pair rather than each row's largest component:
  // a ghost line that outnumbers the real roll in one row still has to find an accomplice in the
  // other row, and a real roll 12 mm away from it does not qualify.
  for (const std::size_t s : {kLeftRoll, kRightRoll}) {
    const std::vector<const Component*> row0_candidates = candidatesFor(0, s);
    const std::vector<const Component*> row1_candidates = candidatesFor(1, s);
    double pair_distance = kNoAnchor;
    bool pairing_degraded = false;

    if (!row0_candidates.empty() && !row1_candidates.empty()) {
      const Component* best0 = nullptr;
      const Component* best1 = nullptr;
      double best_distance = 0.0;
      std::size_t best_slots = 0;
      for (const Component* candidate0 : row0_candidates) {
        const Box box0 = boxOf(*rows[0], *candidate0);
        for (const Component* candidate1 : row1_candidates) {
          const double distance = boxDistance(box0, boxOf(*rows[1], *candidate1));
          const std::size_t slots = candidate0->size() + candidate1->size();
          // Strictly closer wins; an exact tie (both sides of the comparison come out of the same
          // deterministic computation) goes to the larger pair, and a tie there keeps the first
          // pair seen -- row0 candidates outer, row1 inner, both in componentsFor order.
          const bool better = best0 == nullptr || distance < best_distance ||
                              (distance == best_distance && slots > best_slots);
          if (!better) continue;
          best0 = candidate0;
          best1 = candidate1;
          best_distance = distance;
          best_slots = slots;
        }
      }
      if (best0 != nullptr && best1 != nullptr) {
        chosen[0][s] = *best0;
        chosen[1][s] = *best1;
        pair_distance = best_distance;
      }
    } else if (!row0_candidates.empty() || !row1_candidates.empty()) {
      // Only one row labelled this roll: there is nothing to agree with, so fall back to the
      // largest component and flag both rows. A ghost that wins on size here wins unchallenged.
      const std::size_t r = row0_candidates.empty() ? 1 : 0;
      if (const Component* pick = pickLargest(components[r][s], options.min_component_slots)) {
        chosen[r][s] = *pick;
      }
      pairing_degraded = true;
    }

    for (std::size_t r = 0; r < kRows; ++r) {
      record(r, s, kNoAnchor, false, pair_distance, pairing_degraded);
    }
  }

  // Pass 2 -- surfaces, anchored on the roll box pooled across both rows.
  const auto anchorBox = [&](std::size_t s) -> std::optional<Box> {
    std::optional<Box> box;
    for (std::size_t r = 0; r < kRows; ++r) {
      if (chosen[r][s].empty()) continue;
      const Box row_box = boxOf(*rows[r], chosen[r][s]);
      box = box ? unite(*box, row_box) : row_box;
    }
    return box;
  };
  const std::pair<std::size_t, std::size_t> surface_anchor[] = {{kLeftSurface, kLeftRoll},
                                                                {kRightSurface, kRightRoll}};
  for (const auto& pairing : surface_anchor) {
    const std::size_t s = pairing.first;
    const std::optional<Box> anchor = anchorBox(pairing.second);
    for (std::size_t r = 0; r < kRows; ++r) {
      const auto& candidates = components[r][s];
      if (candidates.empty()) {
        record(r, s, kNoAnchor, false);
        continue;
      }
      if (!anchor) {
        // That roll was unlabelled in both rows: nothing to judge against, fall back to size.
        if (const Component* pick = pickLargest(candidates, options.min_component_slots)) {
          chosen[r][s] = *pick;
        }
        record(r, s, kNoAnchor, false);
        continue;
      }

      bool any_above_floor = false;
      for (const auto& candidate : candidates) {
        if (candidate.size() >= options.min_component_slots) any_above_floor = true;
      }

      const Component* best_gated = nullptr;
      double best_gated_distance = 0.0;
      const Component* nearest = nullptr;
      double nearest_distance = std::numeric_limits<double>::infinity();
      for (const auto& candidate : candidates) {
        if (any_above_floor && candidate.size() < options.min_component_slots) continue;
        const double distance = boxDistance(boxOf(*rows[r], candidate), *anchor);
        if (distance < nearest_distance) {
          nearest_distance = distance;
          nearest = &candidate;
        }
        if (distance > options.anchor_gap_mm) continue;
        if (best_gated == nullptr || candidate.size() > best_gated->size()) {
          best_gated = &candidate;
          best_gated_distance = distance;
        }
      }

      if (best_gated != nullptr) {
        chosen[r][s] = *best_gated;
        record(r, s, best_gated_distance, false);
      } else if (nearest != nullptr) {
        // Nothing is contiguous with the roll. Keep the closest component so the segment does not
        // silently vanish, but flag it -- these labels are not trustworthy.
        chosen[r][s] = *nearest;
        record(r, s, nearest_distance, true);
      } else {
        record(r, s, kNoAnchor, false);
      }
    }
  }

  // Rewrite rejected slots to background. Slots carrying any other class, and slots that were
  // never valid, are passed through untouched.
  for (std::size_t r = 0; r < kRows; ++r) {
    std::vector<char> keep(result.labels[r].size(), 0);
    for (std::size_t s = 0; s < kSegmentCount; ++s) {
      for (const std::size_t slot : chosen[r][s]) keep[slot] = 1;
    }
    const auto& valid = *rows[r]->valid;
    for (std::size_t i = 0; i < result.labels[r].size(); ++i) {
      if (keep[i] || !valid[i]) continue;
      for (const auto& segment : kSegments) {
        if (result.labels[r][i] == segment.class_id) {
          result.labels[r][i] = 0;
          break;
        }
      }
    }
  }
  return result;
}

std::string describeRefinements(const std::vector<SegmentRefinement>& refinements) {
  std::string text;
  for (const auto& entry : refinements) {
    if (!text.empty()) text += "; ";
    text += entry.segment + "/row" + std::to_string(entry.row) + ": " +
            std::to_string(entry.component_count) + " components, kept " +
            std::to_string(entry.kept_slots) + " slots, dropped " +
            std::to_string(entry.dropped_slots);
    if (!std::isnan(entry.anchor_distance_mm)) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.2f", entry.anchor_distance_mm);
      text += ", anchor ";
      text += buffer;
      text += "mm";
    }
    if (!std::isnan(entry.pair_distance_mm)) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.2f", entry.pair_distance_mm);
      text += ", pair ";
      text += buffer;
      text += "mm";
    }
    if (entry.anchor_gate_missed) text += " (anchor-gate-missed)";
    if (entry.pairing_degraded) text += " (pairing-degraded)";
  }
  return text;
}

RoiBoxesResult boxesFromRefinedLabels(const SegmentRow& row0, const SegmentRow& row1,
                                      const MaskRefineOptions& options) {
  auto refined = refineSegmentMasks(row0, row1, options);
  SegmentRow refined_row0 = row0;
  SegmentRow refined_row1 = row1;
  refined_row0.labels = &refined.labels[0];
  refined_row1.labels = &refined.labels[1];
  auto result = boxesFromLabels(refined_row0, refined_row1);
  result.refinements = std::move(refined.refinements);
  return result;
}

}  // namespace gap::ml
