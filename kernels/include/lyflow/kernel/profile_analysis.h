#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "lyflow/data.h"

namespace lyflow::kernel::profile_analysis {
using CancelCheck = std::function<bool()>;
using Progress = std::function<void(float)>;
using Matrix4 = std::array<double, 16>;

struct CloudView {
  const std::vector<float>& xyz;
  const std::vector<float>& intensity;
  const std::vector<float>& normals;
  const std::vector<std::uint8_t>& rgb;

  CloudView(const std::vector<float>& positions, const std::vector<float>& strengths,
            const std::vector<float>& directions, const std::vector<std::uint8_t>& colors)
      : xyz(positions), intensity(strengths), normals(directions), rgb(colors) {}
  CloudView(const PointCloud& cloud)
      : xyz(cloud.xyz), intensity(cloud.intensity), normals(cloud.normals), rgb(cloud.rgb) {}
  std::size_t pointCount() const { return xyz.size() / 3; }
  bool hasNormals() const { return !normals.empty(); }
};

struct LineFit {
  std::array<double, 2> center{0, 0}, direction{1, 0};
  double minimum = 0, maximum = 0, rms = 0;
  std::size_t count = 0;
};

struct CloudChannels {
  std::vector<float> xyz, intensity, normals;
  std::vector<std::uint8_t> rgb;
};

bool validateCloud(const CloudView& cloud, std::size_t minimum, std::string& why);
bool fitLine(const CloudView& cloud, int x, int y, double minimum, double maximum, LineFit& output,
             const CancelCheck& cancelled = {});
Line2D nativeLine(const LineFit& fit);
bool rigidMatrix(const Transform& transform, Matrix4& output);
Matrix4 identityMatrix();
Matrix4 compose(const Matrix4& outer, const Matrix4& inner);
bool transformCloud(const CloudView& input, const Matrix4& matrix, CloudChannels& output,
                    const CancelCheck& cancelled = {}, const Progress& progress = {});
} // namespace lyflow::kernel::profile_analysis
