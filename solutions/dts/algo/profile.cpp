#include "algo/profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lyflow::dts {
namespace {
void poll(const kernel::profile::CancelCheck& cancelled) {
  if (cancelled && cancelled())
    throw kernel::profile::Cancelled();
}
} // namespace

bool findDome(const Profile& p, const std::vector<Piece>& pieces, const DomeParams& dp, Dome& out,
              const kernel::profile::CancelCheck& cancelled) {
  poll(cancelled);
  bool found = false;
  double bestScore = -1.0;
  for (const Piece& piece : pieces) {
    poll(cancelled);
    const View v = pieceView(p, piece, cancelled);
    const int n = v.size();
    if (n < 20)
      continue;
    std::vector<double> zb(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
      zb[k] = dp.bulgeSign * v.z(k);
    const int kw = 9, h = kw / 2;
    std::vector<double> zs(static_cast<std::size_t>(n), 0.0);
    for (int k = 0; k < n; ++k) {
      poll(cancelled);
      double acc = 0;
      for (int j = k - h; j <= k + h; ++j)
        acc += zb[std::clamp(j, 0, n - 1)];
      zs[k] = acc / kw;
    }
    for (int pk = 1; pk < n - 1; ++pk) {
      poll(cancelled);
      if (!(zs[pk] > zs[pk - 1] && zs[pk] >= zs[pk + 1]))
        continue;
      int lo = pk;
      double runMin = zs[pk];
      while (lo > 0 && zs[lo - 1] <= runMin + dp.walkTolMm) {
        poll(cancelled);
        --lo;
        runMin = std::min(runMin, zs[lo]);
      }
      int hi = pk;
      runMin = zs[pk];
      while (hi < n - 1 && zs[hi + 1] <= runMin + dp.walkTolMm) {
        poll(cancelled);
        ++hi;
        runMin = std::min(runMin, zs[hi]);
      }
      int loEnd = lo, hiEnd = pk;
      for (int k = lo; k <= pk; ++k) {
        poll(cancelled);
        if (zs[k] < zs[loEnd])
          loEnd = k;
      }
      for (int k = pk; k <= hi; ++k) {
        poll(cancelled);
        if (zs[k] < zs[hiEnd])
          hiEnd = k;
      }
      const double base = std::max(zs[loEnd], zs[hiEnd]);
      const double height = zs[pk] - base;
      const double width = v.x(hiEnd) - v.x(loEnd);
      if (height < dp.minHeightMm || width < dp.minLenMm || width > dp.maxLenMm)
        continue;
      const double top = base + 0.9 * height;
      int a = pk;
      while (a > loEnd && zs[a] > top)
        --a;
      int b = pk;
      while (b < hiEnd && zs[b] > top)
        ++b;
      if (b - a >= 8) {
        double cx = 0, cz = 0, r = 0;
        if (fitCircle(v, a, b, dp.bulgeSign, cx, cz, r, cancelled) && r > dp.maxRadiusMm)
          continue;
      }
      const double score = height * height * width;
      if (score <= bestScore)
        continue;
      bestScore = score;
      found = true;
      out.xPeak = v.x(pk);
      out.zPeak = v.z(pk);
      out.xLo = v.x(loEnd);
      out.xHi = v.x(hiEnd);
      out.heightMm = height;
    }
  }
  return found;
}

bool apexAxis(const View& all, const Dome& dome, double halfMm, double rMin, double rMax,
              ApexAxis& out, const kernel::profile::CancelCheck& cancelled) {
  poll(cancelled);
  const int k0 = all.lowerBound(dome.xPeak - halfMm);
  const int k1 = all.upperBound(dome.xPeak + halfMm) - 1;
  if (k1 - k0 + 1 < 12)
    return false;
  double cx = 0, cz = 0, r = 0;
  if (!fitCircle(all, k0, k1, 1.0, cx, cz, r, cancelled))
    return false;
  if (r < rMin || r > rMax)
    return false;
  out.cx = cx;
  out.cz = cz;
  out.radius = r;
  out.angleDeg = std::atan2(dome.zPeak - cz, dome.xPeak - cx) * 180.0 / 3.14159265358979323846;
  return true;
}

int sealSide(const View& all, const Dome& dome) {
  const int left = all.lowerBound(dome.xLo);
  const int right = all.size() - all.upperBound(dome.xHi);
  return left >= right ? 1 : -1;
}

bool findRoot(const View& all, double lo, double hi, int sealSign, double bulgeSign,
              double promSmall, double promMetal, double bumpLimitX, RootPoint& out,
              const kernel::profile::CancelCheck& cancelled) {
  poll(cancelled);
  if (lo > hi)
    std::swap(lo, hi);
  const int k0 = all.lowerBound(lo);
  const int k1 = all.upperBound(hi) - 1;
  const int n = k1 - k0 + 1;
  if (n < 8)
    return false;
  std::vector<double> v(static_cast<std::size_t>(n));
  for (int k = 0; k < n; ++k)
    v[k] = bulgeSign * all.z(k0 + k);
  const std::vector<Extremum> ext = zigzag(v, promSmall, cancelled);
  int bestK = -1;
  double bestScore = -1e30, bestMetal = 0, bestDome = 0;
  int nBumps = 0;
  for (std::size_t e = 1; e + 1 < ext.size(); ++e) {
    poll(cancelled);
    if (!ext[e].isMax)
      continue;
    const int i = ext[e].k;
    const double dropLeft = v[i] - v[ext[e - 1].k];
    const double dropRight = v[i] - v[ext[e + 1].k];
    const double metalDrop = sealSign > 0 ? dropLeft : dropRight;
    const double domeDrop = sealSign > 0 ? dropRight : dropLeft;
    if (metalDrop < promMetal || domeDrop < promSmall)
      continue;
    if (all.x(k0 + i) * sealSign < bumpLimitX * sealSign)
      continue;
    ++nBumps;
    const double score = all.x(k0 + i) * sealSign;
    if (score > bestScore) {
      bestScore = score;
      bestK = i;
      bestMetal = metalDrop;
      bestDome = domeDrop;
    }
  }
  if (bestK >= 0) {
    out.index = k0 + bestK;
    out.x = all.x(out.index);
    out.z = all.z(out.index);
    out.bump = true;
    out.nBumps = nBumps;
    out.promMetal = bestMetal;
    out.promDome = bestDome;
    return true;
  }
  int argmin = 0;
  for (int k = 1; k < n; ++k) {
    poll(cancelled);
    if (v[k] < v[argmin])
      argmin = k;
  }
  out.index = k0 + argmin;
  out.x = all.x(out.index);
  out.z = all.z(out.index);
  out.bump = false;
  out.nBumps = 0;
  return true;
}

bool pickMetal(const std::vector<Face>& faces, const Dome& dome, const ApexAxis& axis,
               const View& all, const PickParams& pp, MetalPick& out,
               std::vector<MetalPick>* rejected, const kernel::profile::CancelCheck& cancelled) {
  poll(cancelled);
  int best = -1;
  double bestGap = 1e30;
  for (std::size_t i = 0; i < faces.size(); ++i) {
    poll(cancelled);
    const Face& f = faces[i];
    double gap = 0, edge = 0;
    if (pp.sealSign > 0) {
      if (f.x1 > dome.xLo + 0.5)
        continue;
      gap = dome.xLo - f.x1;
      edge = f.x1;
    } else {
      if (f.x0 < dome.xHi - 0.5)
        continue;
      gap = f.x0 - dome.xHi;
      edge = f.x0;
    }
    const double rel = normalizeDeg(f.angleDeg - axis.angleDeg);
    const bool ok = std::fabs(rel - pp.angleToApexDeg) <= pp.angleTolDeg;
    if (ok && gap < bestGap) {
      bestGap = gap;
      best = static_cast<int>(i);
      out.faceIndex = best;
      out.gapMm = gap;
      out.relDeg = rel;
      out.edgeX = edge;
    } else if (rejected != nullptr) {
      MetalPick r;
      r.faceIndex = static_cast<int>(i);
      r.gapMm = gap;
      r.relDeg = rel;
      r.edgeX = edge;
      rejected->push_back(r);
    }
  }
  if (best < 0)
    return false;
  const double lo = pp.sealSign > 0 ? out.edgeX - pp.fitLenMm : out.edgeX;
  const double hi = pp.sealSign > 0 ? out.edgeX : out.edgeX + pp.fitLenMm;
  const int k0 = all.lowerBound(lo);
  const int k1 = all.upperBound(hi) - 1;
  if (k1 - k0 + 1 < 12)
    return false;
  return fitLine(all, k0, k1, out.line, cancelled);
}

} // namespace lyflow::dts
