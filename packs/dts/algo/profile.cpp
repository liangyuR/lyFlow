#include "algo/profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lyflow::dts {
namespace {

bool isValid(float v) { return std::isfinite(v); }

double median(std::vector<double>& v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const std::size_t h = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + h, v.end());
  const double hi = v[h];
  if (v.size() % 2 == 1) return hi;
  std::nth_element(v.begin(), v.begin() + h - 1, v.end());
  return 0.5 * (v[h - 1] + hi);
}

}  // namespace

double normalizeDeg(double a) {
  while (a <= -180.0) a += 360.0;
  while (a > 180.0) a -= 360.0;
  return a;
}

double Line::angleDeg() const { return std::atan2(dz, dx) * 180.0 / 3.14159265358979323846; }

double Line::nx() const { return dx >= 0 ? -dz : dz; }

double Line::nz() const { return dx >= 0 ? dx : -dx; }

double Line::signedDistance(double qx, double qz) const {
  return (qx - px) * nx() + (qz - pz) * nz();
}

int View::lowerBound(double xv) const {
  int lo = 0, hi = size();
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (x(mid) < xv) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

int View::upperBound(double xv) const {
  int lo = 0, hi = size();
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (x(mid) <= xv) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

std::vector<float> movingMedian(const std::vector<float>& z, int width) {
  if (width <= 1) return z;
  const int n = static_cast<int>(z.size());
  const int h = width / 2;
  std::vector<float> out(z.size(), std::numeric_limits<float>::quiet_NaN());
  std::vector<double> win;
  win.reserve(static_cast<std::size_t>(2 * h + 1));
  for (int i = 0; i < n; ++i) {
    if (!isValid(z[i])) continue;
    win.clear();
    for (int j = i - h; j <= i + h; ++j) {
      if (j < 0 || j >= n) continue;
      if (isValid(z[j])) win.push_back(z[j]);
    }
    out[i] = static_cast<float>(median(win));
  }
  return out;
}

std::vector<Piece> splitPieces(const Profile& p, double gapMm, double jumpMm, int minPoints) {
  std::vector<Piece> out;
  const int n = static_cast<int>(p.size());
  if (n == 0) return out;
  int start = 0;
  for (int i = 1; i <= n; ++i) {
    const bool brk = i == n || std::fabs(p.x[i] - p.x[i - 1]) > gapMm ||
                     std::fabs(p.z[i] - p.z[i - 1]) > jumpMm;
    if (!brk) continue;
    if (i - start >= minPoints) out.push_back(Piece{start, i - 1});
    start = i;
  }
  return out;
}

View pieceView(const Profile& p, const Piece& piece) {
  std::vector<int> idx;
  idx.reserve(static_cast<std::size_t>(piece.i1 - piece.i0 + 1));
  for (int i = piece.i0; i <= piece.i1; ++i) idx.push_back(i);
  std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return p.x[a] < p.x[b]; });
  return View(p, std::move(idx));
}

View mergedView(const Profile& p, const std::vector<Piece>& pieces) {
  std::vector<int> idx;
  for (const Piece& q : pieces) {
    for (int i = q.i0; i <= q.i1; ++i) idx.push_back(i);
  }
  std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return p.x[a] < p.x[b]; });
  return View(p, std::move(idx));
}

bool fitLine(const View& v, int k0, int k1, Line& out) {
  const int n = k1 - k0 + 1;
  if (n < 2) return false;
  double sx = 0, sz = 0;
  for (int k = k0; k <= k1; ++k) {
    sx += v.x(k);
    sz += v.z(k);
  }
  const double cx = sx / n, cz = sz / n;
  double sxx = 0, sxz = 0, szz = 0;
  for (int k = k0; k <= k1; ++k) {
    const double dx = v.x(k) - cx, dz = v.z(k) - cz;
    sxx += dx * dx;
    sxz += dx * dz;
    szz += dz * dz;
  }
  const double tr = sxx + szz;
  const double det = sxx * szz - sxz * sxz;
  const double disc = std::sqrt(std::max(tr * tr / 4.0 - det, 0.0));
  const double lam = tr / 2.0 + disc;
  double dx = 0, dz = 0;
  if (std::fabs(sxz) > 1e-12) {
    dx = lam - szz;
    dz = sxz;
  } else if (sxx >= szz) {
    dx = 1;
    dz = 0;
  } else {
    dx = 0;
    dz = 1;
  }
  const double norm = std::hypot(dx, dz);
  if (!(norm > 0)) return false;
  dx /= norm;
  dz /= norm;
  if (dx < 0) {
    dx = -dx;
    dz = -dz;
  }
  const double nxv = -dz, nzv = dx;
  double acc = 0;
  for (int k = k0; k <= k1; ++k) {
    const double r = (v.x(k) - cx) * nxv + (v.z(k) - cz) * nzv;
    acc += r * r;
  }
  out.px = cx;
  out.pz = cz;
  out.dx = dx;
  out.dz = dz;
  out.rms = std::sqrt(acc / n);
  out.n = n;
  return true;
}

bool fitCircle(const View& v, int k0, int k1, double bulgeSign, double& cx, double& cz,
               double& r) {
  const int n = k1 - k0 + 1;
  if (n < 3) return false;
  double a11 = 0, a12 = 0, a13 = 0, a22 = 0, a23 = 0, a33 = 0;
  double b1 = 0, b2 = 0, b3 = 0;
  for (int k = k0; k <= k1; ++k) {
    const double px = v.x(k);
    const double pz = bulgeSign * v.z(k);
    const double c1 = 2 * px, c2 = 2 * pz, c3 = 1.0;
    const double rhs = px * px + pz * pz;
    a11 += c1 * c1;
    a12 += c1 * c2;
    a13 += c1 * c3;
    a22 += c2 * c2;
    a23 += c2 * c3;
    a33 += c3 * c3;
    b1 += c1 * rhs;
    b2 += c2 * rhs;
    b3 += c3 * rhs;
  }
  const double m[3][3] = {{a11, a12, a13}, {a12, a22, a23}, {a13, a23, a33}};
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (std::fabs(det) < 1e-12) return false;
  const double inv[3][3] = {
      {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
       (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
      {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
       (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
      {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
       (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det}};
  const double s0 = inv[0][0] * b1 + inv[0][1] * b2 + inv[0][2] * b3;
  const double s1 = inv[1][0] * b1 + inv[1][1] * b2 + inv[1][2] * b3;
  const double s2 = inv[2][0] * b1 + inv[2][1] * b2 + inv[2][2] * b3;
  cx = s0;
  cz = bulgeSign * s1;
  r = std::sqrt(std::max(s2 + s0 * s0 + s1 * s1, 0.0));
  return true;
}

std::vector<Face> splitFaces(const Profile& p, const std::vector<Piece>& pieces,
                             const FaceParams& fp) {
  std::vector<Face> faces;
  for (const Piece& piece : pieces) {
    const View v = pieceView(p, piece);
    const int n = v.size();
    if (n < 12) continue;
    const double lo = v.x(0), hi = v.x(n - 1);
    if (hi - lo < fp.fitLenMm) continue;
    const double step = std::max(fp.fitLenMm * 0.2, 0.25);

    struct Window {
      bool flat = false;
      double a = 0, b = 0, angle = 0;
    };
    std::vector<Window> windows;
    const int nWin = static_cast<int>(std::floor((hi - fp.fitLenMm + 1e-9 - lo) / step)) + 1;
    for (int wi = 0; wi < nWin; ++wi) {
      const double a = lo + wi * step;
      Window w;
      w.a = a;
      w.b = a + fp.fitLenMm;
      const int k0 = v.lowerBound(a);
      const int k1 = v.upperBound(w.b) - 1;
      Line line;
      if (k1 - k0 + 1 >= 12 && fitLine(v, k0, k1, line) && line.rms < fp.maxRmsMm) {
        w.flat = true;
        w.angle = line.angleDeg();
      }
      windows.push_back(w);
    }
    windows.push_back(Window{});

    std::vector<Window> run;
    for (const Window& w : windows) {
      if (w.flat && (run.empty() || std::fabs(w.angle - run.back().angle) <= fp.angleBreakDeg)) {
        run.push_back(w);
        continue;
      }
      if (!run.empty()) {
        const double a = run.front().a, b = run.back().b;
        const int k0 = v.lowerBound(a);
        const int k1 = v.upperBound(b) - 1;
        Line line;
        if (k1 - k0 + 1 >= 12 && b - a >= fp.minLenMm && fitLine(v, k0, k1, line)) {
          Face f;
          f.x0 = v.x(k0);
          f.x1 = v.x(k1);
          f.z0 = v.z(k0);
          f.z1 = v.z(k1);
          f.lenMm = f.x1 - f.x0;
          f.n = k1 - k0 + 1;
          f.angleDeg = line.angleDeg();
          f.rmsMm = line.rms;
          f.line = line;
          faces.push_back(f);
        }
      }
      run.clear();
      if (w.flat) run.push_back(w);
    }
  }
  std::stable_sort(faces.begin(), faces.end(),
                   [](const Face& a, const Face& b) { return a.x0 < b.x0; });
  return faces;
}

bool findDome(const Profile& p, const std::vector<Piece>& pieces, const DomeParams& dp,
              Dome& out) {
  bool found = false;
  double bestScore = -1.0;
  for (const Piece& piece : pieces) {
    const View v = pieceView(p, piece);
    const int n = v.size();
    if (n < 20) continue;
    std::vector<double> zb(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) zb[k] = dp.bulgeSign * v.z(k);
    const int kw = 9, h = kw / 2;
    std::vector<double> zs(static_cast<std::size_t>(n), 0.0);
    for (int k = 0; k < n; ++k) {
      double acc = 0;
      for (int j = k - h; j <= k + h; ++j) acc += zb[std::clamp(j, 0, n - 1)];
      zs[k] = acc / kw;
    }
    for (int pk = 1; pk < n - 1; ++pk) {
      if (!(zs[pk] > zs[pk - 1] && zs[pk] >= zs[pk + 1])) continue;
      int lo = pk;
      double runMin = zs[pk];
      while (lo > 0 && zs[lo - 1] <= runMin + dp.walkTolMm) {
        --lo;
        runMin = std::min(runMin, zs[lo]);
      }
      int hi = pk;
      runMin = zs[pk];
      while (hi < n - 1 && zs[hi + 1] <= runMin + dp.walkTolMm) {
        ++hi;
        runMin = std::min(runMin, zs[hi]);
      }
      int loEnd = lo, hiEnd = pk;
      for (int k = lo; k <= pk; ++k) {
        if (zs[k] < zs[loEnd]) loEnd = k;
      }
      for (int k = pk; k <= hi; ++k) {
        if (zs[k] < zs[hiEnd]) hiEnd = k;
      }
      const double base = std::max(zs[loEnd], zs[hiEnd]);
      const double height = zs[pk] - base;
      const double width = v.x(hiEnd) - v.x(loEnd);
      if (height < dp.minHeightMm || width < dp.minLenMm || width > dp.maxLenMm) continue;
      const double top = base + 0.9 * height;
      int a = pk;
      while (a > loEnd && zs[a] > top) --a;
      int b = pk;
      while (b < hiEnd && zs[b] > top) ++b;
      if (b - a >= 8) {
        double cx = 0, cz = 0, r = 0;
        if (fitCircle(v, a, b, dp.bulgeSign, cx, cz, r) && r > dp.maxRadiusMm) continue;
      }
      const double score = height * height * width;
      if (score <= bestScore) continue;
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
              ApexAxis& out) {
  const int k0 = all.lowerBound(dome.xPeak - halfMm);
  const int k1 = all.upperBound(dome.xPeak + halfMm) - 1;
  if (k1 - k0 + 1 < 12) return false;
  double cx = 0, cz = 0, r = 0;
  if (!fitCircle(all, k0, k1, 1.0, cx, cz, r)) return false;
  if (r < rMin || r > rMax) return false;
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

std::vector<Extremum> zigzag(const std::vector<double>& v, double prom) {
  std::vector<Extremum> out;
  const int n = static_cast<int>(v.size());
  if (n < 3) return out;
  int lo = 0, hi = 0, mode = 0;
  for (int i = 1; i < n; ++i) {
    if (v[i] > v[hi]) hi = i;
    if (v[i] < v[lo]) lo = i;
    if (mode >= 0 && v[hi] - v[i] >= prom) {
      out.push_back(Extremum{hi, true});
      mode = -1;
      lo = i;
    } else if (mode <= 0 && v[i] - v[lo] >= prom) {
      out.push_back(Extremum{lo, false});
      mode = 1;
      hi = i;
    }
  }
  return out;
}

bool findRoot(const View& all, double lo, double hi, int sealSign, double bulgeSign,
              double promSmall, double promMetal, double bumpLimitX, RootPoint& out) {
  if (lo > hi) std::swap(lo, hi);
  const int k0 = all.lowerBound(lo);
  const int k1 = all.upperBound(hi) - 1;
  const int n = k1 - k0 + 1;
  if (n < 8) return false;
  std::vector<double> v(static_cast<std::size_t>(n));
  for (int k = 0; k < n; ++k) v[k] = bulgeSign * all.z(k0 + k);
  const std::vector<Extremum> ext = zigzag(v, promSmall);
  int bestK = -1;
  double bestScore = -1e30, bestMetal = 0, bestDome = 0;
  int nBumps = 0;
  for (std::size_t e = 1; e + 1 < ext.size(); ++e) {
    if (!ext[e].isMax) continue;
    const int i = ext[e].k;
    const double dropLeft = v[i] - v[ext[e - 1].k];
    const double dropRight = v[i] - v[ext[e + 1].k];
    const double metalDrop = sealSign > 0 ? dropLeft : dropRight;
    const double domeDrop = sealSign > 0 ? dropRight : dropLeft;
    if (metalDrop < promMetal || domeDrop < promSmall) continue;
    if (all.x(k0 + i) * sealSign < bumpLimitX * sealSign) continue;
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
    if (v[k] < v[argmin]) argmin = k;
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
               std::vector<MetalPick>* rejected) {
  int best = -1;
  double bestGap = 1e30;
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const Face& f = faces[i];
    double gap = 0, edge = 0;
    if (pp.sealSign > 0) {
      if (f.x1 > dome.xLo + 0.5) continue;
      gap = dome.xLo - f.x1;
      edge = f.x1;
    } else {
      if (f.x0 < dome.xHi - 0.5) continue;
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
  if (best < 0) return false;
  const double lo = pp.sealSign > 0 ? out.edgeX - pp.fitLenMm : out.edgeX;
  const double hi = pp.sealSign > 0 ? out.edgeX : out.edgeX + pp.fitLenMm;
  const int k0 = all.lowerBound(lo);
  const int k1 = all.upperBound(hi) - 1;
  if (k1 - k0 + 1 < 12) return false;
  return fitLine(all, k0, k1, out.line);
}

}  // namespace lyflow::dts
