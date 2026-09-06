#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "domain/detection/DetectionConfiguration.hpp"

namespace gap::ml {

struct ClusterBounds {
  double lo = 0.0;
  double hi = 0.0;
  std::size_t trimmed = 0;
};

// Port of ml_handoff/annotation/corrections.py robust_cluster_bounds: the closed interval
// containing the median, walking outward from the median until the first adjacent gap exceeds
// gap_factor * max(1.4826 * MAD, scale_floor). `values` fewer than min_points long is returned
// verbatim as (min, max, 0) -- too small a sample to judge which cluster is the body. Throws
// std::runtime_error if `values` is empty.
ClusterBounds robustClusterBounds(std::vector<double> values, double gap_factor, double scale_floor,
                                  std::size_t min_points);

// One camera row's (x, z, valid, labels) view, all the same length. `labels` holds the argmax
// class id per slot (see onnx_port_contract.md section 2/3 for the class-id -> segment mapping).
struct SegmentRow {
  const std::vector<double>* x_mm = nullptr;
  const std::vector<double>* z_mm = nullptr;
  const std::vector<std::uint8_t>* valid = nullptr;
  const std::vector<int>* labels = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Mask refinement ("refine-v1"). NOT part of the original corrections.py port -- it is a stage
// that runs *before* boxesFromLabels' mask/pool/robustClusterBounds chain and can only ever remove
// labelled slots, never add them. See docs/model_roi_port_contract.md section 5 for the exact
// semantics the ML side must mirror.
//
// Why it exists: robustClusterBounds is a marginal 1-D gap walk over *sorted* values, so it scales
// its cut threshold by the segment's own MAD. For a long surface segment that makes the x
// threshold roughly 3.7x the segment length (never triggers) and pins the z threshold at the
// 1.2 mm scale floor x gap_factor = 12 mm. A mislabelled but *coherent* line inside that blind
// zone therefore survives the walk and inflates the surface box. Refinement discriminates on
// connectivity and on proximity to the (independently labelled, empirically reliable) roll
// segment instead of on density or on 1-D spread.
struct MaskRefineOptions {
  bool enabled = true;
  // Two consecutive labelled slots start a new run when either their slot indices are farther
  // apart than split_slot_gap or their Euclidean step exceeds split_step_mm. Deliberately eager:
  // over-splitting is repaired by the linkage pass below, under-splitting is not recoverable.
  double split_step_mm = 1.0;
  std::size_t split_slot_gap = 16;
  // Single-linkage merge of runs whose axis-aligned bounding boxes are within link_mm. Re-joins
  // one physical feature that an interleaved outlier or a dropout tore into several runs.
  double link_mm = 3.0;
  // Components smaller than this are discarded outright (the "discrete points" case). Applied
  // only when at least one component survives it.
  std::size_t min_component_slots = 4;
  // A surface component is accepted when it comes within this distance of its roll anchor box.
  double anchor_gap_mm = 5.0;

  // The refinement disabled, i.e. boxesFromRefinedLabels() degenerates to boxesFromLabels().
  static MaskRefineOptions contractExact() {
    MaskRefineOptions options;
    options.enabled = false;
    return options;
  }
};

// One (row, segment) refinement outcome. Only emitted when the refinement actually had something
// to decide -- more than one component, or slots dropped, or a suspicion flag was raised -- so an
// untouched profile produces an empty vector and callers can log unconditionally.
struct SegmentRefinement {
  std::string segment;
  std::size_t row = 0;
  std::size_t component_count = 0;
  std::size_t kept_slots = 0;
  std::size_t dropped_slots = 0;
  // Surfaces only: distance from the kept component to the roll anchor box. NaN when no anchor
  // was available (that roll segment was itself unlabelled in both rows).
  double anchor_distance_mm = 0.0;
  // No component fell within anchor_gap_mm; the nearest one was kept anyway. A strong signal that
  // this profile's labels are untrustworthy -- worth surfacing rather than silently accepting.
  bool anchor_gate_missed = false;
  // Rolls only: box separation of the winning cross-row pair. NaN for surfaces and whenever the
  // pairing could not run (one row empty, or both).
  double pair_distance_mm = std::numeric_limits<double>::quiet_NaN();
  // Rolls only: one row had no candidate at all, so that row's roll fell back to the largest
  // component with no cross-row agreement to check it. Both rows of the segment carry the flag.
  bool pairing_degraded = false;
};

struct MaskRefineResult {
  // Refined per-row labels: rejected slots are rewritten to 0 (background), every other slot is
  // passed through untouched.
  std::array<std::vector<int>, 2> labels;
  std::vector<SegmentRefinement> refinements;
};

// Runs the refinement over both rows. With options.enabled == false this returns the input labels
// verbatim and no diagnostics.
MaskRefineResult refineSegmentMasks(const SegmentRow& row0, const SegmentRow& row1,
                                    const MaskRefineOptions& options);

// One-line, log-friendly rendering of the refinement diagnostics; empty string for empty input, so
// callers can test it instead of the vector. Entries whose anchor gate was missed are marked
// "anchor-gate-missed" and entries whose roll pairing degraded are marked "pairing-degraded" --
// those two are worth alerting on, the rest are informational.
std::string describeRefinements(const std::vector<SegmentRefinement>& refinements);

struct RoiBoxesResult {
  detection::domain::TemplateRoiConfiguration rois;
  // Segment names (left_surface/left_roll/right_roll/right_surface, contract order) whose pooled
  // labeled slot count across both rows fell below MIN_SEGMENT_SLOTS.
  std::vector<std::string> missing_segments;
  // Empty unless boxesFromRefinedLabels() ran the refinement and it had a decision to make.
  std::vector<SegmentRefinement> refinements;
};

// Port of ml_handoff/annotation/corrections.py boxes_from_labels, specialized to exactly the two
// rows (primary, secondary) gap_ml works with, with robust_z=robust_x=True (the only mode used
// here) and the contract's fixed class ids / MIN_SEGMENT_SLOTS / gap_factor / scale floors.
RoiBoxesResult boxesFromLabels(const SegmentRow& row0, const SegmentRow& row1);

// refineSegmentMasks() followed by boxesFromLabels() on the refined labels, with the refinement's
// diagnostics carried through in RoiBoxesResult::refinements. This is the runtime entry point;
// boxesFromLabels() above stays the bit-exact port that the golden vectors pin down.
RoiBoxesResult boxesFromRefinedLabels(const SegmentRow& row0, const SegmentRow& row1,
                                      const MaskRefineOptions& options = {});

}  // namespace gap::ml
