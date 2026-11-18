#include "lyflow_cv/color_model.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

#include <opencv2/imgproc.hpp>

namespace lyflow::std_image::color {
namespace {

using Json = nlohmann::json;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// [a + add, b; b, c + add] 的逆，存成 [xx, xy, yy]。奇异时返回 false。
bool inverse2(const std::array<double, 3>& m, double add, std::array<double, 3>* out) {
  const double a = m[0] + add, b = m[1], c = m[2] + add;
  const double det = a * c - b * b;
  if (!(det > 1e-300))
    return false;
  *out = {c / det, -b / det, a / det};
  return true;
}

double quad(const std::array<double, 3>& inv, double dx, double dy) {
  return inv[0] * dx * dx + 2.0 * inv[1] * dx * dy + inv[2] * dy * dy;
}

double sigmoid(double x) {
  if (x >= 0)
    return 1.0 / (1.0 + std::exp(-std::min(x, 700.0)));
  const double e = std::exp(std::max(x, -700.0));
  return e / (1.0 + e);
}

cv::Rect imageRect(const cv::Mat& m) { return cv::Rect(0, 0, m.cols, m.rows); }

cv::Rect regionOf(const cv::Mat& m, cv::Rect roi) {
  if (roi.width <= 0 && roi.height <= 0)
    return imageRect(m);
  return roi & imageRect(m);
}

int chromaBin(double v) {
  const double w = 2.0 * ColorStats::kChromaRange / ColorStats::kChromaBins;
  const int i = static_cast<int>(std::floor((v + ColorStats::kChromaRange) / w));
  return std::clamp(i, 0, ColorStats::kChromaBins - 1);
}

double binCenter(int i) {
  const double w = 2.0 * ColorStats::kChromaRange / ColorStats::kChromaBins;
  return -ColorStats::kChromaRange + (i + 0.5) * w;
}

int lumaBin(double y) {
  return std::clamp(static_cast<int>(std::floor(y / 4.0)), 0, ColorStats::kLumaBins - 1);
}

/// 直方图（每格 4 级）的第 q 百分位，格内线性插值。
double histPercentile(const std::vector<double>& h, double q) {
  double total = 0;
  for (double v : h)
    total += v;
  if (!(total > 0))
    return kNaN;
  const double want = q / 100.0 * total;
  double acc = 0;
  for (std::size_t i = 0; i < h.size(); ++i) {
    if (h[i] > 0 && acc + h[i] >= want)
      return 4.0 * (static_cast<double>(i) + (want - acc) / h[i]);
    acc += h[i];
  }
  return 4.0 * static_cast<double>(h.size());
}

/// 背景直方图上的确定性 EM：先按「计数 × 到已选中心的最小距离²」挑初始中心，硬分 10 轮，再软 EM 30 轮。
std::vector<Gauss2> fitBackground(const ColorStats& st, int maxK) {
  struct Bin {
    double x, y, n, sum;
  };
  std::vector<Bin> bins;
  double total = 0;
  for (int i = 0; i < ColorStats::kChromaBins; ++i) {
    for (int j = 0; j < ColorStats::kChromaBins; ++j) {
      const std::size_t at = static_cast<std::size_t>(i * ColorStats::kChromaBins + j);
      const double n = st.bgHist[at];
      if (n > 0) {
        bins.push_back(Bin{binCenter(i), binCenter(j), n, st.bgSumRGB[at]});
        total += n;
      }
    }
  }
  std::vector<Gauss2> out;
  if (bins.empty() || maxK < 1)
    return out;
  const double w = 2.0 * ColorStats::kChromaRange / ColorStats::kChromaBins;
  const double reg = w * w / 12.0 + 1e-6;

  std::vector<std::array<double, 2>> centers;
  std::size_t first = 0;
  for (std::size_t b = 1; b < bins.size(); ++b) {
    if (bins[b].n > bins[first].n)
      first = b;
  }
  centers.push_back({bins[first].x, bins[first].y});
  while (static_cast<int>(centers.size()) < maxK) {
    double best = 0;
    std::size_t pick = 0;
    for (std::size_t b = 0; b < bins.size(); ++b) {
      double d2 = std::numeric_limits<double>::infinity();
      for (const auto& c : centers)
        d2 = std::min(d2, (bins[b].x - c[0]) * (bins[b].x - c[0]) +
                              (bins[b].y - c[1]) * (bins[b].y - c[1]));
      if (bins[b].n * d2 > best) {
        best = bins[b].n * d2;
        pick = b;
      }
    }
    if (!(best > 0))
      break;
    centers.push_back({bins[pick].x, bins[pick].y});
  }
  const std::size_t K = centers.size();
  std::vector<std::size_t> assign(bins.size(), 0);
  for (int it = 0; it < 10; ++it) {
    for (std::size_t b = 0; b < bins.size(); ++b) {
      double best = std::numeric_limits<double>::infinity();
      for (std::size_t k = 0; k < K; ++k) {
        const double d2 = (bins[b].x - centers[k][0]) * (bins[b].x - centers[k][0]) +
                          (bins[b].y - centers[k][1]) * (bins[b].y - centers[k][1]);
        if (d2 < best) {
          best = d2;
          assign[b] = k;
        }
      }
    }
    for (std::size_t k = 0; k < K; ++k) {
      double n = 0, sx = 0, sy = 0;
      for (std::size_t b = 0; b < bins.size(); ++b) {
        if (assign[b] == k) {
          n += bins[b].n;
          sx += bins[b].n * bins[b].x;
          sy += bins[b].n * bins[b].y;
        }
      }
      if (n > 0)
        centers[k] = {sx / n, sy / n};
    }
  }
  std::vector<Gauss2> g(K);
  for (std::size_t k = 0; k < K; ++k) {
    double n = 0, xx = 0, xy = 0, yy = 0;
    for (std::size_t b = 0; b < bins.size(); ++b) {
      if (assign[b] != k)
        continue;
      const double dx = bins[b].x - centers[k][0], dy = bins[b].y - centers[k][1];
      n += bins[b].n;
      xx += bins[b].n * dx * dx;
      xy += bins[b].n * dx * dy;
      yy += bins[b].n * dy * dy;
    }
    g[k].weight = n / total;
    g[k].mean = centers[k];
    g[k].cov = n > 0 ? std::array<double, 3>{xx / n + reg, xy / n, yy / n + reg}
                     : std::array<double, 3>{reg, 0, reg};
  }
  std::vector<double> resp(bins.size() * K);
  for (int it = 0; it < 30; ++it) {
    for (std::size_t b = 0; b < bins.size(); ++b) {
      double lmax = -std::numeric_limits<double>::infinity();
      for (std::size_t k = 0; k < K; ++k) {
        std::array<double, 3> inv;
        double l = -std::numeric_limits<double>::infinity();
        if (g[k].weight > 0 && inverse2(g[k].cov, 0, &inv)) {
          const double det = g[k].cov[0] * g[k].cov[2] - g[k].cov[1] * g[k].cov[1];
          l = std::log(g[k].weight) - 0.5 * std::log(det) -
              0.5 * quad(inv, bins[b].x - g[k].mean[0], bins[b].y - g[k].mean[1]);
        }
        resp[b * K + k] = l;
        lmax = std::max(lmax, l);
      }
      double sum = 0;
      for (std::size_t k = 0; k < K; ++k) {
        const double e = std::isfinite(resp[b * K + k]) ? std::exp(resp[b * K + k] - lmax) : 0.0;
        resp[b * K + k] = e;
        sum += e;
      }
      for (std::size_t k = 0; k < K; ++k)
        resp[b * K + k] = sum > 0 ? resp[b * K + k] / sum : 0.0;
    }
    for (std::size_t k = 0; k < K; ++k) {
      double n = 0, sx = 0, sy = 0, si = 0;
      for (std::size_t b = 0; b < bins.size(); ++b) {
        const double r = bins[b].n * resp[b * K + k];
        n += r;
        sx += r * bins[b].x;
        sy += r * bins[b].y;
        si += resp[b * K + k] * bins[b].sum;
      }
      if (!(n > 1e-9)) {
        g[k].weight = 0;
        continue;
      }
      const double mx = sx / n, my = sy / n;
      double xx = 0, xy = 0, yy = 0;
      for (std::size_t b = 0; b < bins.size(); ++b) {
        const double r = bins[b].n * resp[b * K + k];
        const double dx = bins[b].x - mx, dy = bins[b].y - my;
        xx += r * dx * dx;
        xy += r * dx * dy;
        yy += r * dy * dy;
      }
      g[k].weight = n / total;
      g[k].mean = {mx, my};
      g[k].cov = {xx / n + reg, xy / n, yy / n + reg};
      g[k].intensity = si / n;
    }
  }
  double kept = 0;
  for (const Gauss2& x : g) {
    if (x.weight >= 0.02)
      kept += x.weight;
  }
  for (const Gauss2& x : g) {
    if (x.weight >= 0.02) {
      out.push_back(x);
      out.back().weight = x.weight / kept;
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Gauss2& a, const Gauss2& b) { return a.weight > b.weight; });
  return out;
}

Json covJson(const std::array<double, 3>& c) {
  return Json::array({Json::array({c[0], c[1]}), Json::array({c[1], c[2]})});
}

Json gaussJson(const Gauss2& g) {
  return Json{{"weight", g.weight},
              {"mean", Json::array({g.mean[0], g.mean[1]})},
              {"cov", covJson(g.cov)},
              {"intensity", g.intensity}};
}

bool num(const Json& j, const char* k, double* out) {
  const auto it = j.find(k);
  if (it == j.end() || !it->is_number())
    return false;
  *out = it->get<double>();
  return std::isfinite(*out);
}

bool vec2Of(const Json& j, std::array<double, 2>* out) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
    return false;
  *out = {j[0].get<double>(), j[1].get<double>()};
  return std::isfinite((*out)[0]) && std::isfinite((*out)[1]);
}

bool covOf(const Json& j, std::array<double, 3>* out) {
  std::array<double, 2> r0, r1;
  if (!j.is_array() || j.size() != 2 || !vec2Of(j[0], &r0) || !vec2Of(j[1], &r1))
    return false;
  if (r0[1] != r1[0])
    return false;
  *out = {r0[0], r0[1], r1[1]};
  return r0[0] > 0 && r1[1] > 0 && r0[0] * r1[1] - r0[1] * r0[1] > 0;
}

bool gaussOf(const Json& j, Gauss2* g, bool needWeight) {
  if (!j.is_object() || !vec2Of(j.value("mean", Json()), &g->mean) ||
      !covOf(j.value("cov", Json()), &g->cov) || !num(j, "intensity", &g->intensity) ||
      !(g->intensity > 0))
    return false;
  if (needWeight)
    return num(j, "weight", &g->weight) && g->weight > 0;
  return true;
}

void normChroma(double a1, double a2, double* r, double* b) {
  const double e1 = std::exp(a1), e2 = std::exp(a2), s = e1 + 1.0 + e2;
  *r = e1 / s;
  *b = e2 / s;
}

} // namespace

bool isDark(const ColorSpec& spec, double luma) {
  return spec.noiseK / std::max(luma, 1.0) > spec.noiseMax;
}

void ColorStats::add(const ColorStats& o) {
  glueN += o.glueN;
  for (int i = 0; i < 2; ++i)
    glueSum[i] += o.glueSum[i];
  for (int i = 0; i < 3; ++i)
    glueSq[i] += o.glueSq[i];
  glueSumRGB += o.glueSumRGB;
  for (std::size_t i = 0; i < glueLuma.size(); ++i)
    glueLuma[i] += o.glueLuma[i];
  bgN += o.bgN;
  for (std::size_t i = 0; i < bgHist.size(); ++i) {
    bgHist[i] += o.bgHist[i];
    bgSumRGB[i] += o.bgSumRGB[i];
  }
  bgGlueLike += o.bgGlueLike;
  saturated += o.saturated;
  dark += o.dark;
  frames += o.frames;
}

ColorModel fitColorModel(const ColorStats& stats, const ColorSpec& spec, int maxComponents) {
  ColorModel m;
  m.spec = spec;
  m.stats = stats;
  if (stats.glueN > 0) {
    const double n = stats.glueN;
    const double mx = stats.glueSum[0] / n, my = stats.glueSum[1] / n;
    m.glue.mean = {mx, my};
    m.glue.cov = {std::max(stats.glueSq[0] / n - mx * mx, 0.0) + 1e-6,
                  stats.glueSq[1] / n - mx * my,
                  std::max(stats.glueSq[2] / n - my * my, 0.0) + 1e-6};
    const double lim = std::sqrt(m.glue.cov[0] * m.glue.cov[2]) * (1 - 1e-9);
    m.glue.cov[1] = std::clamp(m.glue.cov[1], -lim, lim);
    m.glue.intensity = stats.glueSumRGB / n;
  }
  m.glueLuma = {histPercentile(stats.glueLuma, 5), histPercentile(stats.glueLuma, 50),
                histPercentile(stats.glueLuma, 95)};
  m.background = fitBackground(stats, maxComponents);
  return m;
}

void accumulateColorStats(ColorStats& st, const ColorSpec& spec,
                          const std::vector<ColorSample>& glue, const std::vector<ColorSample>& bg,
                          double trimSigma, double bgExcludeSigma) {
  // 胶样本截尾：先拟合一次，马氏距离超过 trimSigma 的去掉（胶边混进来的、偏离的）
  std::vector<const ColorSample*> kept;
  Gauss2 g0;
  if (!glue.empty()) {
    double n = 0, sx = 0, sy = 0;
    for (const ColorSample& p : glue) {
      n += 1;
      sx += p.c1;
      sy += p.c2;
    }
    g0.mean = {sx / n, sy / n};
    double xx = 0, xy = 0, yy = 0;
    for (const ColorSample& p : glue) {
      const double dx = p.c1 - g0.mean[0], dy = p.c2 - g0.mean[1];
      xx += dx * dx;
      xy += dx * dy;
      yy += dy * dy;
    }
    g0.cov = {xx / n, xy / n, yy / n};
    std::array<double, 3> inv;
    const double floor2 = spec.chromaFloor * spec.chromaFloor;
    const bool ok = inverse2(g0.cov, floor2, &inv);
    for (const ColorSample& p : glue) {
      if (!ok || quad(inv, p.c1 - g0.mean[0], p.c2 - g0.mean[1]) <= trimSigma * trimSigma)
        kept.push_back(&p);
    }
    if (kept.size() < 2) {
      kept.clear();
      for (const ColorSample& p : glue)
        kept.push_back(&p);
    }
  }
  for (const ColorSample* p : kept) {
    st.glueN += 1;
    st.glueSum[0] += p->c1;
    st.glueSum[1] += p->c2;
    st.glueSq[0] += p->c1 * p->c1;
    st.glueSq[1] += p->c1 * p->c2;
    st.glueSq[2] += p->c2 * p->c2;
    st.glueSumRGB += p->sum;
    st.glueLuma[static_cast<std::size_t>(lumaBin(p->luma))] += 1;
  }
  // 背景带里像胶的样本（旁边另一道胶、示教线偏到胶上）不进背景
  const ColorModel gm = fitColorModel(st, spec, 0);
  std::array<double, 3> ginv;
  const bool gOk =
      st.glueN >= 2 && inverse2(gm.glue.cov, spec.chromaFloor * spec.chromaFloor, &ginv);
  for (const ColorSample& p : bg) {
    if (gOk && quad(ginv, p.c1 - gm.glue.mean[0], p.c2 - gm.glue.mean[1]) <
                   bgExcludeSigma * bgExcludeSigma) {
      st.bgGlueLike += 1;
      continue;
    }
    const std::size_t at =
        static_cast<std::size_t>(chromaBin(p.c1) * ColorStats::kChromaBins + chromaBin(p.c2));
    st.bgN += 1;
    st.bgHist[at] += 1;
    st.bgSumRGB[at] += p.sum;
  }
}

ColorScorer::ColorScorer(const ColorModel& m) : model_(m) {
  normChroma(m.glue.mean[0], m.glue.mean[1], &gr_, &gb_);
  for (const Gauss2& b : m.background) {
    Part q;
    q.pooled = {0.5 * (m.glue.cov[0] + b.cov[0]), 0.5 * (m.glue.cov[1] + b.cov[1]),
                0.5 * (m.glue.cov[2] + b.cov[2])};
    q.ex = b.mean[0] - m.glue.mean[0];
    q.ey = b.mean[1] - m.glue.mean[1];
    normChroma(b.mean[0], b.mean[1], &q.br, &q.bb);
    q.rho = m.glue.intensity > 0 && b.intensity > 0 ? m.glue.intensity / b.intensity : 1.0;
    parts_.push_back(q);
  }
}

double ColorScorer::score(const ColorSample& sample, ColorState* state) const {
  const double c1 = sample.c1, c2 = sample.c2, luma = sample.luma;
  const ColorModel& m = model_;
  const ColorSpec& sp = m.spec;
  if (sample.saturated) {
    *state = ColorState::Highlight;
    return kNaN;
  }
  // 明显比见过的最暗的胶还暗，就不是胶（不管色度噪声大不大）；够不上这么暗、色度又看不清的才是无信息
  if (std::isfinite(m.glueLuma[0]) && luma < sp.darkRatio * m.glueLuma[0]) {
    *state = ColorState::DarkBackground;
    return 0.0;
  }
  if (isDark(sp, luma)) {
    *state = ColorState::Dark;
    return kNaN;
  }
  *state = ColorState::Info;
  const double noise = sp.noiseK / std::max(luma, 1.0);
  const double add = sp.chromaFloor * sp.chromaFloor + noise * noise;
  const double gx = c1 - m.glue.mean[0], gy = c2 - m.glue.mean[1];
  // 马氏距离（合并协方差 + 噪声）最近的「胶—背景」连线段：像素按它解混
  const Part* best = nullptr;
  double gate2 = std::numeric_limits<double>::infinity();
  for (const Part& q : parts_) {
    std::array<double, 3> inv;
    if (!inverse2(q.pooled, add, &inv))
      continue;
    const double ee = quad(inv, q.ex, q.ey);
    double u = 0;
    if (ee > 0)
      u = std::clamp((inv[0] * gx * q.ex + inv[1] * (gx * q.ey + gy * q.ex) + inv[2] * gy * q.ey) /
                         ee,
                     0.0, 1.0);
    const double d2 = quad(inv, gx - u * q.ex, gy - u * q.ey);
    if (d2 < gate2) {
      gate2 = d2;
      best = &q;
    }
  }
  if (!best)
    return 0.0;
  // 归一化色度 (r, b) = (R, B) / (R + G + B) 上，混合像素是两类按光通量 β 的线性组合
  const double dr = gr_ - best->br, db = gb_ - best->bb;
  const double dd = dr * dr + db * db;
  const double beta =
      dd > 0
          ? std::clamp(((sample.nr - best->br) * dr + (sample.nb - best->bb) * db) / dd, 0.0, 1.0)
          : 0.0;
  // 光通量占比 → 面积占比：亮胶压在暗底上，一点胶就把色度拉过去
  const double denom = beta + best->rho * (1.0 - beta);
  const double alpha = denom > 0 ? beta / denom : 0.0;
  return alpha * sigmoid(0.5 * (sp.glueGate * sp.glueGate - gate2));
}

double colorSeparation(const ColorModel& m) {
  double best = kNaN;
  const double add = m.spec.chromaFloor * m.spec.chromaFloor;
  for (const Gauss2& b : m.background) {
    const std::array<double, 3> pooled{0.5 * (m.glue.cov[0] + b.cov[0]),
                                       0.5 * (m.glue.cov[1] + b.cov[1]),
                                       0.5 * (m.glue.cov[2] + b.cov[2])};
    std::array<double, 3> inv;
    if (!inverse2(pooled, add, &inv))
      continue;
    const double d = std::sqrt(quad(inv, b.mean[0] - m.glue.mean[0], b.mean[1] - m.glue.mean[1]));
    if (!(best <= d))
      best = d;
  }
  return best;
}

// ------------------------------------------------------------------ 色度平面与得分图

namespace {

/// 平滑 RGB（f32）与膨胀后的高光掩膜，覆盖 outer；inner 在 outer 里的位置是 local。
struct Smoothed {
  cv::Mat f, sat;
  cv::Rect inner, local;
};

Smoothed smoothed(const cv::Mat& rgb, const ColorSpec& spec, cv::Rect roi) {
  Smoothed out;
  out.inner = regionOf(rgb, roi);
  if (out.inner.empty())
    return out;
  const cv::Rect& inner = out.inner;
  const int r = spec.blurSigma > 0 ? static_cast<int>(std::ceil(3.0 * spec.blurSigma)) : 0;
  const int d = std::max(spec.satDilate, 0);
  const int pad = r + d + 2;
  const cv::Rect outer =
      cv::Rect(inner.x - pad, inner.y - pad, inner.width + 2 * pad, inner.height + 2 * pad) &
      imageRect(rgb);
  const cv::Mat src = rgb(outer).clone();
  src.convertTo(out.f, CV_32FC3);
  if (r > 0)
    cv::GaussianBlur(out.f, out.f, cv::Size(2 * r + 1, 2 * r + 1), spec.blurSigma, spec.blurSigma,
                     cv::BORDER_REFLECT_101);
  std::vector<cv::Mat> ch;
  cv::split(src, ch);
  out.sat = cv::max(cv::max(ch[0], ch[1]), ch[2]);
  cv::compare(out.sat, spec.satLevel, out.sat, cv::CMP_GE);
  if (d > 0)
    cv::dilate(out.sat, out.sat,
               cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * d + 1, 2 * d + 1)));
  out.local = cv::Rect(inner.tl() - outer.tl(), inner.size());
  return out;
}

/// 一个像素的特征：对数色度、BT.601 亮度、R + G + B、归一化色度 (R + 1, B + 1) / (R + G + B + 3)
/// （与 exp(c1)、exp(c2) 归一化是同一个数，打分时省掉两次 exp）。
struct Feature {
  double c1, c2, luma, sum, nr, nb;
};

Feature featureOf(const cv::Vec3f& p) {
  const double R = p[0], G = p[1], B = p[2];
  const double s3 = R + G + B + 3.0;
  return Feature{std::log((R + 1.0) / (G + 1.0)),
                 std::log((B + 1.0) / (G + 1.0)),
                 0.299 * R + 0.587 * G + 0.114 * B,
                 R + G + B,
                 (R + 1.0) / s3,
                 (B + 1.0) / s3};
}

/// [0, rows) 按 16 行一段分给 threads 个线程；每行只写自己的那几格，结果与线程数无关。
template <class Body> void forRows(int rows, int threads, Body&& body) {
  constexpr int kBlock = 16;
  const int blocks = (rows + kBlock - 1) / kBlock;
  const int count = std::clamp(threads, 1, std::max(blocks, 1));
  std::atomic<int> next{0};
  auto work = [&] {
    for (int b = next.fetch_add(1); b < blocks; b = next.fetch_add(1)) {
      for (int y = b * kBlock; y < std::min(rows, (b + 1) * kBlock); ++y)
        body(y);
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < count; ++t)
    pool.emplace_back(work);
  work();
  for (std::thread& th : pool)
    th.join();
}

} // namespace

ColorPlanes colorPlanes(const cv::Mat& rgb, const ColorSpec& spec, cv::Rect roi) {
  ColorPlanes out;
  const Smoothed sm = smoothed(rgb, spec, roi);
  if (sm.inner.empty())
    return out;
  out.frame = sm.inner;
  out.saturated = sm.sat(sm.local).clone();
  for (cv::Mat* m : {&out.c1, &out.c2, &out.luma, &out.sum, &out.nr, &out.nb})
    m->create(sm.inner.size(), CV_32F);
  for (int y = 0; y < sm.inner.height; ++y) {
    const cv::Vec3f* p = sm.f.ptr<cv::Vec3f>(y + sm.local.y) + sm.local.x;
    for (int x = 0; x < sm.inner.width; ++x) {
      const Feature q = featureOf(p[x]);
      out.c1.ptr<float>(y)[x] = static_cast<float>(q.c1);
      out.c2.ptr<float>(y)[x] = static_cast<float>(q.c2);
      out.luma.ptr<float>(y)[x] = static_cast<float>(q.luma);
      out.sum.ptr<float>(y)[x] = static_cast<float>(q.sum);
      out.nr.ptr<float>(y)[x] = static_cast<float>(q.nr);
      out.nb.ptr<float>(y)[x] = static_cast<float>(q.nb);
    }
  }
  return out;
}

ColorMaps colorMaps(const cv::Mat& rgb, const ColorModel& m, cv::Rect roi, int threads) {
  ColorMaps out;
  const Smoothed sm = smoothed(rgb, m.spec, roi);
  if (sm.inner.empty())
    return out;
  out.frame = sm.inner;
  out.score.create(sm.inner.size(), CV_32F);
  out.state.create(sm.inner.size(), CV_32F);
  out.luma.create(sm.inner.size(), CV_32F);
  const ColorScorer scorer(m);
  // 打分与 colorPlanes + ColorScorer 是同一组式子（特征先舍成 float 再打分），只是一遍做完、不落中间图
  forRows(sm.inner.height, threads, [&](int y) {
    const cv::Vec3f* p = sm.f.ptr<cv::Vec3f>(y + sm.local.y) + sm.local.x;
    const std::uint8_t* s = sm.sat.ptr<std::uint8_t>(y + sm.local.y) + sm.local.x;
    float* sc = out.score.ptr<float>(y);
    float* st = out.state.ptr<float>(y);
    float* l = out.luma.ptr<float>(y);
    for (int x = 0; x < sm.inner.width; ++x) {
      const Feature q = featureOf(p[x]);
      const float c1 = static_cast<float>(q.c1), c2 = static_cast<float>(q.c2);
      const float luma = static_cast<float>(q.luma);
      ColorState state;
      sc[x] =
          static_cast<float>(scorer.score(ColorSample{c1, c2, luma, q.sum, static_cast<float>(q.nr),
                                                      static_cast<float>(q.nb), s[x] != 0},
                                          &state));
      st[x] = static_cast<float>(static_cast<int>(state));
      l[x] = luma;
    }
  });
  return out;
}

Json colorModelJson(const ColorModel& m) {
  const ColorSpec& sp = m.spec;
  Json bins = Json::array();
  for (int i = 0; i < ColorStats::kChromaBins; ++i) {
    for (int j = 0; j < ColorStats::kChromaBins; ++j) {
      const std::size_t at = static_cast<std::size_t>(i * ColorStats::kChromaBins + j);
      const double n = m.stats.bgHist[at];
      if (n > 0)
        bins.push_back(Json::array({i, j, n, m.stats.bgSumRGB[at]}));
    }
  }
  Json bg = Json::array();
  for (const Gauss2& g : m.background)
    bg.push_back(gaussJson(g));
  const auto lumaOrNull = [](double v) { return std::isfinite(v) ? Json(v) : Json(); };
  return Json{{"version", 1},
              {"space", "log_chroma"},
              {"spec",
               {{"blurSigma", sp.blurSigma},
                {"satLevel", sp.satLevel},
                {"satDilate", sp.satDilate},
                {"noiseK", sp.noiseK},
                {"noiseMax", sp.noiseMax},
                {"darkRatio", sp.darkRatio},
                {"chromaFloor", sp.chromaFloor},
                {"glueGate", sp.glueGate}}},
              {"glue",
               {{"n", m.stats.glueN},
                {"mean", Json::array({m.glue.mean[0], m.glue.mean[1]})},
                {"cov", covJson(m.glue.cov)},
                {"intensity", m.glue.intensity},
                {"luma", Json::array({lumaOrNull(m.glueLuma[0]), lumaOrNull(m.glueLuma[1]),
                                      lumaOrNull(m.glueLuma[2])})}}},
              {"background", std::move(bg)},
              {"stats",
               {{"frames", m.stats.frames},
                {"glue",
                 {{"n", m.stats.glueN},
                  {"sum", Json::array({m.stats.glueSum[0], m.stats.glueSum[1]})},
                  {"sumsq", Json::array({m.stats.glueSq[0], m.stats.glueSq[1], m.stats.glueSq[2]})},
                  {"sumRGB", m.stats.glueSumRGB},
                  {"luma", m.stats.glueLuma}}},
                {"background",
                 {{"n", m.stats.bgN},
                  {"binWidth", 2.0 * ColorStats::kChromaRange / ColorStats::kChromaBins},
                  {"range", Json::array({-ColorStats::kChromaRange, ColorStats::kChromaRange})},
                  {"bins", std::move(bins)}}},
                {"glueLike", m.stats.bgGlueLike},
                {"saturated", m.stats.saturated},
                {"dark", m.stats.dark}}}};
}

bool readColorModel(const Json& j, ColorModel* out, std::string* why) {
  const auto fail = [&](const std::string& w) {
    if (why)
      *why = w;
    return false;
  };
  if (!j.is_object())
    return fail("不是 JSON 对象");
  if (j.value("version", Json()) != Json(1) || j.value("space", Json()) != Json("log_chroma"))
    return fail("version 要是 1、space 要是 log_chroma");
  ColorModel m;
  const Json sp = j.value("spec", Json());
  double satDilate = 0;
  if (!sp.is_object() || !num(sp, "blurSigma", &m.spec.blurSigma) ||
      !num(sp, "satLevel", &m.spec.satLevel) || !num(sp, "satDilate", &satDilate) ||
      !num(sp, "noiseK", &m.spec.noiseK) || !num(sp, "noiseMax", &m.spec.noiseMax) ||
      !num(sp, "darkRatio", &m.spec.darkRatio) || !num(sp, "chromaFloor", &m.spec.chromaFloor) ||
      !num(sp, "glueGate", &m.spec.glueGate))
    return fail("spec 缺字段或不是有限的数");
  m.spec.satDilate = static_cast<int>(satDilate);
  if (m.spec.blurSigma < 0 || m.spec.blurSigma > 5 || m.spec.satDilate < 0 ||
      m.spec.satDilate > 20 || !(m.spec.noiseK >= 0) || !(m.spec.noiseMax > 0) ||
      m.spec.chromaFloor <= 0 || m.spec.glueGate <= 0)
    return fail("spec 的取值超出范围");
  const Json g = j.value("glue", Json());
  if (!gaussOf(g, &m.glue, false))
    return fail("glue 的 mean / cov / intensity 不合格（cov 要对称正定、intensity > 0）");
  const Json luma = g.value("luma", Json());
  if (!luma.is_array() || luma.size() != 3)
    return fail("glue.luma 要是 [p5, p50, p95]");
  for (int i = 0; i < 3; ++i)
    m.glueLuma[i] = luma[i].is_number() ? luma[i].get<double>() : kNaN;
  const Json bg = j.value("background", Json());
  if (!bg.is_array() || bg.empty() || bg.size() > 3)
    return fail("background 要有 1–3 个分量");
  for (const Json& b : bg) {
    Gauss2 x;
    if (!gaussOf(b, &x, true))
      return fail("background 的分量不合格（weight > 0、cov 对称正定、intensity > 0）");
    m.background.push_back(x);
  }
  // 生产用的精简模型可以不带 stats（只能打分，不能当 prior 累加）
  if (!j.contains("stats") || j["stats"].is_null()) {
    m.hasStats = false;
    *out = std::move(m);
    return true;
  }
  const Json st = j.value("stats", Json());
  const Json sg = st.value("glue", Json());
  const Json sb = st.value("background", Json());
  if (!st.is_object() || !sg.is_object() || !sb.is_object())
    return fail("stats 缺 glue / background");
  std::array<double, 2> sum{0, 0};
  const Json sq = sg.value("sumsq", Json());
  const Json lh = sg.value("luma", Json());
  const Json bins = sb.value("bins", Json());
  if (!num(sg, "n", &m.stats.glueN) || !vec2Of(sg.value("sum", Json()), &sum) || !sq.is_array() ||
      sq.size() != 3 || !lh.is_array() || lh.size() != ColorStats::kLumaBins ||
      !num(sg, "sumRGB", &m.stats.glueSumRGB) || !num(sb, "n", &m.stats.bgN) || !bins.is_array())
    return fail("stats 的字段不合格");
  m.stats.glueSum = sum;
  for (int i = 0; i < 3; ++i) {
    if (!sq[i].is_number())
      return fail("stats.glue.sumsq 不是数");
    m.stats.glueSq[i] = sq[i].get<double>();
  }
  for (int i = 0; i < ColorStats::kLumaBins; ++i) {
    if (!lh[i].is_number() || lh[i].get<double>() < 0)
      return fail("stats.glue.luma 不是非负数");
    m.stats.glueLuma[i] = lh[i].get<double>();
  }
  for (const Json& b : bins) {
    if (!b.is_array() || b.size() != 4 || !b[0].is_number_integer() || !b[1].is_number_integer() ||
        !b[2].is_number() || !b[3].is_number())
      return fail("stats.background.bins 要是 [[i, j, count, sumRGB], …]");
    const int i = b[0].get<int>(), k = b[1].get<int>();
    if (i < 0 || k < 0 || i >= ColorStats::kChromaBins || k >= ColorStats::kChromaBins ||
        !(b[2].get<double>() > 0))
      return fail("stats.background.bins 越界");
    m.stats.bgHist[static_cast<std::size_t>(i * ColorStats::kChromaBins + k)] += b[2].get<double>();
    m.stats.bgSumRGB[static_cast<std::size_t>(i * ColorStats::kChromaBins + k)] +=
        b[3].get<double>();
  }
  double v = 0;
  m.stats.bgGlueLike = num(st, "glueLike", &v) ? v : 0;
  m.stats.saturated = num(st, "saturated", &v) ? v : 0;
  m.stats.dark = num(st, "dark", &v) ? v : 0;
  m.stats.frames = num(st, "frames", &v) ? static_cast<int>(v) : 0;
  *out = std::move(m);
  return true;
}

} // namespace lyflow::std_image::color
