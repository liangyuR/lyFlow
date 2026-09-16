#pragma once
#include <cstddef>
#include <vector>

namespace lyflow::dts {

struct Profile {
  std::vector<float> x;
  std::vector<float> z;

  std::size_t size() const { return x.size(); }
};

struct Piece {
  int i0 = 0;
  int i1 = 0;
};

class View {
 public:
  View() = default;
  View(const Profile& p, std::vector<int> idx) : p_(&p), idx_(std::move(idx)) {}

  int size() const { return static_cast<int>(idx_.size()); }
  double x(int k) const { return p_->x[idx_[k]]; }
  double z(int k) const { return p_->z[idx_[k]]; }
  int index(int k) const { return idx_[k]; }
  const std::vector<int>& indices() const { return idx_; }

  int lowerBound(double xv) const;
  int upperBound(double xv) const;

 private:
  const Profile* p_ = nullptr;
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

struct Dome {
  double xPeak = 0, zPeak = 0;
  double xLo = 0, xHi = 0;
  double heightMm = 0;
};

struct ApexAxis {
  double cx = 0, cz = 0;
  double radius = 0;
  double angleDeg = 0;
};

struct FaceParams {
  double fitLenMm = 5.0;
  double maxRmsMm = 0.05;
  double angleBreakDeg = 6.0;
  double minLenMm = 3.0;
};

struct DomeParams {
  double bulgeSign = 1.0;
  double minLenMm = 5.0;
  double maxLenMm = 30.0;
  double minHeightMm = 1.5;
  double maxRadiusMm = 20.0;
  double walkTolMm = 0.15;
};

struct Extremum {
  int k = 0;
  bool isMax = false;
};

struct RootPoint {
  double x = 0, z = 0;
  int index = 0;
  int nBumps = 0;
  bool bump = false;
  double promMetal = 0, promDome = 0;
};

struct MetalPick {
  int faceIndex = -1;
  double gapMm = 0;
  double relDeg = 0;
  double edgeX = 0;
  Line line;
};

struct PickParams {
  double angleToApexDeg = -55.0;
  double angleTolDeg = 40.0;
  double fitLenMm = 5.0;
  int sealSign = 1;
};

std::vector<float> movingMedian(const std::vector<float>& z, int width);

std::vector<Extremum> zigzag(const std::vector<double>& v, double prom);

bool findRoot(const View& all, double lo, double hi, int sealSign, double bulgeSign,
              double promSmall, double promMetal, double bumpLimitX, RootPoint& out);

bool pickMetal(const std::vector<Face>& faces, const Dome& dome, const ApexAxis& axis,
               const View& all, const PickParams& pp, MetalPick& out,
               std::vector<MetalPick>* rejected);

std::vector<Piece> splitPieces(const Profile& p, double gapMm, double jumpMm, int minPoints);

View pieceView(const Profile& p, const Piece& piece);
View mergedView(const Profile& p, const std::vector<Piece>& pieces);

bool fitLine(const View& v, int k0, int k1, Line& out);
bool fitCircle(const View& v, int k0, int k1, double bulgeSign, double& cx, double& cz, double& r);

std::vector<Face> splitFaces(const Profile& p, const std::vector<Piece>& pieces,
                             const FaceParams& fp);

bool findDome(const Profile& p, const std::vector<Piece>& pieces, const DomeParams& dp, Dome& out);

bool apexAxis(const View& all, const Dome& dome, double halfMm, double rMin, double rMax,
              ApexAxis& out);

int sealSide(const View& all, const Dome& dome);

double normalizeDeg(double a);

}  // namespace lyflow::dts
