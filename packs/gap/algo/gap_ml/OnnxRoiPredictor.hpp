#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gap_ml/RoiBoxes.hpp"
#include "gap_ml/RoiFeatures.hpp"

namespace gap::ml {

// CPU-only, single-threaded (SetIntraOpNumThreads(1), deterministic) ONNX inference of the ROI
// segmentation model, wired straight into buildChannels/boxesFromLabels. The onnxruntime headers
// are kept out of this file on purpose (pimpl) so consumers of RoiFeatures/RoiBoxes don't need to
// see them.
class OnnxRoiPredictor {
 public:
  // Loads and validates the model at construction time; throws std::runtime_error if the file is
  // missing or onnxruntime fails to load it.
  explicit OnnxRoiPredictor(const std::filesystem::path& model);
  ~OnnxRoiPredictor();

  OnnxRoiPredictor(const OnnxRoiPredictor&) = delete;
  OnnxRoiPredictor& operator=(const OnnxRoiPredictor&) = delete;

  // primary is row 0, secondary is row 1 (matches boxesFromLabels' "row 0 first" pooling order).
  // Runs both rows as a single batch=2 inference; ties in the per-slot class argmax resolve to
  // the smallest class index, matching np.argmax. Ort::Session::Run is not thread-safe, so
  // concurrent calls on the same predictor serialize internally.
  //
  // Boxes come from boxesFromRefinedLabels: mask refinement is ON by default, because a
  // mislabelled-but-coherent line in a surface segment survives robustClusterBounds and drags the
  // surface box with it. Pass MaskRefineOptions::contractExact() to get the bit-exact
  // corrections.py behaviour instead (what the golden-vector tier asserts against).
  RoiBoxesResult predict(const ProfileRow& primary, const ProfileRow& secondary,
                         const MaskRefineOptions& refine = {}) const;

  // Same inference as predict(), stopping short of boxesFromLabels: exposed so the golden-vector
  // test can check the ONNX pipeline's per-slot argmax labels independently of boxesFromLabels'
  // own (separately tested) correctness. Not part of the original S2 class sketch -- added
  // because the three-tier golden comparison needs label-level equality, not just boxes; see the
  // report for this deviation.
  std::array<std::vector<int>, 2> predictLabels(const ProfileRow& primary,
                                                const ProfileRow& secondary) const;

  const std::string& modelSha256() const;
  const std::filesystem::path& modelPath() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gap::ml
