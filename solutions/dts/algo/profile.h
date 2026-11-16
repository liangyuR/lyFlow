#pragma once
#include "lyflow/kernel/profile.h"

namespace lyflow::dts {

using Profile = kernel::profile::PlanarSamples;
using kernel::profile::Extremum;
using kernel::profile::Face;
using kernel::profile::FaceParams;
using kernel::profile::fitCircle;
using kernel::profile::fitLine;
using kernel::profile::Line;
using kernel::profile::mergedView;
using kernel::profile::movingMedian;
using kernel::profile::normalizeDeg;
using kernel::profile::Piece;
using kernel::profile::pieceView;
using kernel::profile::splitFaces;
using kernel::profile::splitPieces;
using kernel::profile::View;
using kernel::profile::zigzag;

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

struct DomeParams {
  double bulgeSign = 1.0;
  double minLenMm = 5.0;
  double maxLenMm = 30.0;
  double minHeightMm = 1.5;
  double maxRadiusMm = 20.0;
  double walkTolMm = 0.15;
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

bool findRoot(const View& all, double lo, double hi, int sealSign, double bulgeSign,
              double promSmall, double promMetal, double bumpLimitX, RootPoint& out,
              const kernel::profile::CancelCheck& cancelled = {});

bool pickMetal(const std::vector<Face>& faces, const Dome& dome, const ApexAxis& axis,
               const View& all, const PickParams& pp, MetalPick& out,
               std::vector<MetalPick>* rejected,
               const kernel::profile::CancelCheck& cancelled = {});

bool findDome(const Profile& p, const std::vector<Piece>& pieces, const DomeParams& dp, Dome& out,
              const kernel::profile::CancelCheck& cancelled = {});

bool apexAxis(const View& all, const Dome& dome, double halfMm, double rMin, double rMax,
              ApexAxis& out, const kernel::profile::CancelCheck& cancelled = {});

int sealSide(const View& all, const Dome& dome);

} // namespace lyflow::dts
