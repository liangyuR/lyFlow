#include "gap_ml/OnnxRoiPredictor.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "gap_io/Sha256.hpp"

namespace gap::ml {

struct OnnxRoiPredictor::Impl {
  explicit Impl(const std::filesystem::path& model_path)
      : path(model_path),
        sha256(gap::io::sha256File(model_path)),
        env(ORT_LOGGING_LEVEL_WARNING, "xyz_gap_ml"),
        session(makeSession(env, model_path)) {
    auto in_name = session.GetInputNameAllocated(0, allocator);
    auto out_name = session.GetOutputNameAllocated(0, allocator);
    input_name = in_name.get();
    output_name = out_name.get();
  }

  static Ort::Session makeSession(Ort::Env& env, const std::filesystem::path& model_path) {
    if (!std::filesystem::exists(model_path)) {
      throw std::runtime_error("gap_ml::OnnxRoiPredictor: model file does not exist: " +
                               model_path.string());
    }
    Ort::SessionOptions options;
    // Determinism over throughput: this predictor is a golden-vector equivalence target, not a
    // production throughput path.
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    try {
      return Ort::Session(env, model_path.c_str(), options);
    } catch (const Ort::Exception& error) {
      throw std::runtime_error("gap_ml::OnnxRoiPredictor: failed to load model " +
                               model_path.string() + ": " + error.what());
    }
  }

  std::array<std::vector<int>, 2> predictLabels(const ProfileRow& primary,
                                                const ProfileRow& secondary) {
    const std::vector<float> channels0 = buildChannels(primary);
    const std::vector<float> channels1 = buildChannels(secondary);

    std::vector<float> input(channels0.size() + channels1.size());
    std::copy(channels0.begin(), channels0.end(), input.begin());
    std::copy(channels1.begin(), channels1.end(), input.begin() + channels0.size());

    const std::array<int64_t, 3> input_shape{2, static_cast<int64_t>(kNumChannels),
                                             static_cast<int64_t>(kProfileSlots)};
    const Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input.data(), input.size(), input_shape.data(), input_shape.size());

    const char* input_names[] = {input_name.c_str()};
    const char* output_names[] = {output_name.c_str()};

    std::vector<Ort::Value> outputs;
    {
      // Ort::Session::Run is not thread-safe across concurrent calls on the same session; this
      // predictor is meant to be reused (one session, many predict() calls), so serialize here
      // rather than forcing every caller to hold an external lock.
      std::lock_guard<std::mutex> lock(mutex);
      outputs =
          session.Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);
    }
    if (outputs.empty() || !outputs.front().IsTensor()) {
      throw std::runtime_error("gap_ml::OnnxRoiPredictor: model produced no output tensor");
    }

    const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape[0] != 2 || shape[2] != static_cast<int64_t>(kProfileSlots)) {
      throw std::runtime_error("gap_ml::OnnxRoiPredictor: unexpected logits tensor shape");
    }
    const int64_t num_classes = shape[1];
    const float* logits = outputs.front().GetTensorData<float>();

    std::vector<int> labels0(kProfileSlots);
    std::vector<int> labels1(kProfileSlots);
    auto argmaxInto = [&](int64_t batch_index, std::vector<int>& labels) {
      const float* base = logits + batch_index * num_classes * static_cast<int64_t>(kProfileSlots);
      for (int64_t slot = 0; slot < static_cast<int64_t>(kProfileSlots); ++slot) {
        int best_class = 0;
        float best_value = base[slot];
        for (int64_t class_index = 1; class_index < num_classes; ++class_index) {
          const float value = base[class_index * static_cast<int64_t>(kProfileSlots) + slot];
          // Strict '>' keeps the first (smallest) index on ties, matching np.argmax.
          if (value > best_value) {
            best_value = value;
            best_class = static_cast<int>(class_index);
          }
        }
        labels[static_cast<std::size_t>(slot)] = best_class;
      }
    };
    argmaxInto(0, labels0);
    argmaxInto(1, labels1);

    return {std::move(labels0), std::move(labels1)};
  }

  RoiBoxesResult predict(const ProfileRow& primary, const ProfileRow& secondary,
                         const MaskRefineOptions& refine) {
    const auto labels = predictLabels(primary, secondary);
    const SegmentRow row0{&primary.x_mm, &primary.z_mm, &primary.valid, &labels[0]};
    const SegmentRow row1{&secondary.x_mm, &secondary.z_mm, &secondary.valid, &labels[1]};
    return boxesFromRefinedLabels(row0, row1, refine);
  }

  std::filesystem::path path;
  std::string sha256;
  Ort::Env env;
  Ort::Session session;
  Ort::AllocatorWithDefaultOptions allocator;
  std::string input_name;
  std::string output_name;
  std::mutex mutex;
};

OnnxRoiPredictor::OnnxRoiPredictor(const std::filesystem::path& model)
    : impl_(std::make_unique<Impl>(model)) {}

OnnxRoiPredictor::~OnnxRoiPredictor() = default;

RoiBoxesResult OnnxRoiPredictor::predict(const ProfileRow& primary, const ProfileRow& secondary,
                                         const MaskRefineOptions& refine) const {
  return impl_->predict(primary, secondary, refine);
}

std::array<std::vector<int>, 2> OnnxRoiPredictor::predictLabels(const ProfileRow& primary,
                                                                const ProfileRow& secondary) const {
  return impl_->predictLabels(primary, secondary);
}

const std::string& OnnxRoiPredictor::modelSha256() const { return impl_->sha256; }

const std::filesystem::path& OnnxRoiPredictor::modelPath() const { return impl_->path; }

}  // namespace gap::ml
