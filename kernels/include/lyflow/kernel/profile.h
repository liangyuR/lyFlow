#pragma once
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lyflow::kernel::profile {

// x/z name the selected plane coordinates in millimeters, not implicit world XYZ axes.
struct PlanarSamples {
  std::vector<float> x;
  std::vector<float> z;

  std::size_t size() const { return x.size(); }
};

struct Piece {
  int i0 = 0;
  int i1 = 0;
};

// Indices refer to the borrowed samples; only the view reorders them, never the source buffers.
class View {
public:
  View() = default;
  View(const PlanarSamples& p, std::vector<int> idx);

  int size() const { return static_cast<int>(idx_.size()); }
  double x(int k) const { return p_->x[idx_[k]]; }
  double z(int k) const { return p_->z[idx_[k]]; }
  int index(int k) const { return idx_[k]; }
  const std::vector<int>& indices() const { return idx_; }

  int lowerBound(double xv) const;
  int upperBound(double xv) const;

private:
  const PlanarSamples* p_ = nullptr;
  std::vector<int> idx_;
};

struct Line {
  double px = 0, pz = 0;
  double dx = 1, dz = 0;
  double rms = 0;
  int n = 0;

  double angleDeg() const;
  double nx() const;
  double nz() const;
  double signedDistance(double qx, double qz) const;
};

struct Face {
  double x0 = 0, x1 = 0, z0 = 0, z1 = 0;
  double lenMm = 0;
  double angleDeg = 0;
  double rmsMm = 0;
  int n = 0;
  Line line;
};

struct FaceParams {
  double fitLenMm = 5.0;
  double maxRmsMm = 0.05;
  double angleBreakDeg = 6.0;
  double minLenMm = 3.0;
};

struct Extremum {
  int k = 0;
  bool isMax = false;
};

double normalizeDeg(double a);

using CancelCheck = std::function<bool()>;
class Cancelled : public std::runtime_error {
public:
  Cancelled() : std::runtime_error("Profile computation cancelled") {}
};

// A true cancellation callback throws Cancelled; caller-owned outputs remain unchanged.
std::vector<float> movingMedian(const std::vector<float>& z, int width,
                                const CancelCheck& cancelled = {});
std::vector<Piece> splitPieces(const PlanarSamples& p, double gapMm, double jumpMm, int minPoints,
                               const CancelCheck& cancelled = {});
View pieceView(const PlanarSamples& p, const Piece& piece, const CancelCheck& cancelled = {});
View mergedView(const PlanarSamples& p, const std::vector<Piece>& pieces,
                const CancelCheck& cancelled = {});
bool fitLine(const View& v, int k0, int k1, Line& out, const CancelCheck& cancelled = {});
bool fitCircle(const View& v, int k0, int k1, double bulgeSign, double& cx, double& cz, double& r,
               const CancelCheck& cancelled = {});
std::vector<Face> splitFaces(const PlanarSamples& p, const std::vector<Piece>& pieces,
                             const FaceParams& fp, const CancelCheck& cancelled = {});
std::vector<Extremum> zigzag(const std::vector<double>& v, double prom,
                             const CancelCheck& cancelled = {});

} // namespace lyflow::kernel::profile
