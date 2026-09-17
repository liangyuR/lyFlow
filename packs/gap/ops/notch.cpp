// gap.notch_width：两件软装的圆边贴合成 V 形缝、缝底没有槽时的开口宽度。
//
// gap.groove_joint 假定缝底有一条比两侧面都深的槽，用「两侧面平均线下方 gapDepth 处的槽宽」
// 量间隙。V 缝闭合时没有这条槽：最深点就是两侧圆边碰到一起的那个角，两侧的面又不在同一高度，
// 于是 groove_joint 的水平面要么切不到点、要么切在另一台相机的阴影上，读数在 0.1 与 1.3 之间乱跳
// （罗石 点 1，51 次重复 std 0.35 mm）。这里换一套只依赖基准侧翼面的做法：
//
//   1. 每台相机分开算。被另一件遮住缝底的相机看不到对面的立边，只会量出自己的阴影 ——
//      这种测点用 camera 只选看得见缝底的那一台，而不是把两台平均。
//   2. 锚点 = 框里最深的几个点（x 取中位数）。闭合时它是两圆边相碰的角，张开时是缝底。
//   3. 基准翼面 = 锚点一侧 [baseNear, baseFar] 窗口里的点拟出的直线。只拟平的那一段，避开圆边。
//   4. 在基准线下方 levelDepth 处切一刀（平行于基准线），从锚点向基准侧沿轮廓走到穿出这一刀的
//      A 点 —— 它是基准侧圆边上一个固定的位置；再以 A 点的高度为水平刀口，向对面走到轮廓升回这个
//      高度的 B 点（都线性插值到亚采样精度；孤立的浅点当噪声跳过）。A、B 的水平距离就是开口宽度。
//      对面那一刀必须是水平的：对面沿基准面滑开 g 时，它相对倾斜刀口的深度会少 k·g，读数就不再严格加 g。
//
// 缝闭合时读数是两侧圆边在该深度的固有宽度，用 gapOffset 归零；缝张开 g，读数就加 g。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "gap_ops.h"
#include "std_bridge.hpp"

namespace lyflow::packs::gap {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string mmText(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", mm);
  return buf;
}

nlohmann::json numberOrNull(double v) {
  return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json();
}

struct Sample {
  double x = kNaN;
  double y = kNaN;
};

nlohmann::json sampleMm(const Sample& s) {
  if (!std::isfinite(s.x) || !std::isfinite(s.y)) return nlohmann::json();
  return nlohmann::json::array({mToMm(s.x), mToMm(s.y)});
}

/// 基准翼面直线（米）。n 指向更深的一侧（y 更大），depth() > 0 表示点在面的下方。
struct FlankLine {
  Eigen::Vector2d p{0.0, 0.0};
  Eigen::Vector2d d{1.0, 0.0};
  Eigen::Vector2d n{0.0, 1.0};
  double lo = 0.0;
  double hi = 0.0;
  std::size_t pointCount = 0;
  std::size_t inlierCount = 0;
  double rmsMm = kNaN;
  double maxMm = kNaN;

  double depth(const Sample& s) const { return (Eigen::Vector2d(s.x, s.y) - p).dot(n); }
  double slope() const { return d.y() / d.x(); }
  double yAt(double x) const { return p.y() + (x - p.x()) * slope(); }
};

struct CameraNotch {
  bool valid = false;
  std::string error;
  std::string message;
  std::size_t pointCount = 0;
  Sample anchor;
  double anchorDepth = kNaN;
  double levelY = kNaN;  // 水平刀口的高度（米）= 基准侧穿出点的 y
  Sample start;          // 两圆边相碰的角：参照点到锚点之间相对基准线最深的点
  int iterations = 0;
  bool baseOk = false;
  FlankLine base;
  Sample left;
  Sample right;
  double width = kNaN;  // 米
  // 面差：另一侧翼面线（参照 B 点一侧的窗口）相对基准线在缝心处的垂距。
  bool refOk = false;
  std::string refError;
  FlankLine ref;
  double xMid = kNaN;      // 缝心 = A、B 中点的 x（米）
  double flushRaw = kNaN;  // 米，+ = 非 flushBase 那一侧更高
};

struct NotchParams {
  bool baseLeft = true;
  double levelM = 0.0;
  std::size_t deepestCount = 3;
  double nearM = 0.0;
  double farM = 0.0;
  double lineDistM = 0.0;
  std::size_t minLinePoints = 8;
  int confirm = 2;
  bool flushBaseLeft = true;  // 面差以哪一侧为基准
  double refNearM = 0.0;
  double refFarM = 0.0;
};

void fail(CameraNotch& r, const char* code, std::string message) {
  r.valid = false;
  r.error = code;
  r.message = std::move(message);
}

CameraNotch measureCamera(const GapCloud& cloud, const NotchParams& P) {
  CameraNotch r;
  std::vector<Sample> pts;
  pts.reserve(cloud.size());
  for (const auto& q : cloud) {
    if (std::isfinite(q.x) && std::isfinite(q.y)) pts.push_back({q.x, q.y});
  }
  std::sort(pts.begin(), pts.end(), [](const Sample& a, const Sample& b) { return a.x < b.x; });
  r.pointCount = pts.size();
  if (pts.size() < 20) {
    fail(r, "insufficient_points", "这台相机在框里只有 " + std::to_string(pts.size()) + " 个点");
    return r;
  }

  // 锚点：最深的几个点里离它们 x 中位数最近的那一个。
  std::vector<std::size_t> order(pts.size());
  std::iota(order.begin(), order.end(), 0);
  const std::size_t k = std::max<std::size_t>(1, std::min(P.deepestCount, pts.size()));
  std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k), order.end(),
                    [&](std::size_t a, std::size_t b) { return pts[a].y > pts[b].y; });
  std::vector<double> xs;
  xs.reserve(k);
  for (std::size_t i = 0; i < k; ++i) xs.push_back(pts[order[i]].x);
  std::sort(xs.begin(), xs.end());
  const double xMed = (k % 2 == 1) ? xs[k / 2] : 0.5 * (xs[k / 2 - 1] + xs[k / 2]);
  std::size_t i0 = order[0];
  for (std::size_t i = 0; i < k; ++i) {
    if (std::fabs(pts[order[i]].x - xMed) < std::fabs(pts[i0].x - xMed)) i0 = order[i];
  }
  r.anchor = pts[i0];

  const int baseDir = P.baseLeft ? -1 : +1;
  const auto n = static_cast<std::ptrdiff_t>(pts.size());

  // 基准侧的初始参照：从锚点向基准侧走，第一个比锚点浅 levelDepth 的点。不管锚点落在两圆边相碰的
  // 角上、对面立边的脚下（缝张开时它可能更深）还是缝底，这个点都在基准侧圆边上 —— 拟合窗口以它
  // 为参照，就不会跟着对面滑动而咬到圆边或缝。
  std::ptrdiff_t iRef = -1;
  for (auto i = static_cast<std::ptrdiff_t>(i0) + baseDir; i >= 0 && i < n; i += baseDir) {
    if (pts[static_cast<std::size_t>(i)].y < r.anchor.y - P.levelM) {
      iRef = i;
      break;
    }
  }
  if (iRef < 0) {
    fail(r, "notch_edge_not_found",
         "从最深点向基准侧走到框边，轮廓都没有比最深点浅 " + mmText(mToMm(P.levelM)) + " mm");
    return r;
  }
  const std::ptrdiff_t rangeLo = std::min<std::ptrdiff_t>(iRef, static_cast<std::ptrdiff_t>(i0));
  const std::ptrdiff_t rangeHi = std::max<std::ptrdiff_t>(iRef, static_cast<std::ptrdiff_t>(i0));

  // 一侧翼面：参照点 refX 往 leftSide 那一边 [near, far] 的窗口里拟直线。失败时 *error 是错误码。
  const auto fitFlank = [&](double refX, bool leftSide, double nearM, double farM, FlankLine* line,
                            std::string* error, std::string* message) {
    const double lo = leftSide ? refX - farM : refX + nearM;
    const double hi = leftSide ? refX - nearM : refX + farM;
    GapCloud window;
    for (const auto& s : pts) {
      if (s.x > lo && s.x < hi) {
        GapPoint q;
        q.x = static_cast<float>(s.x);
        q.y = static_cast<float>(s.y);
        q.z = 0.0F;
        window.push_back(q);
      }
    }
    window.width = static_cast<std::uint32_t>(window.size());
    window.height = 1;
    *line = FlankLine{};
    line->lo = lo;
    line->hi = hi;
    line->pointCount = window.size();
    if (window.size() < P.minLinePoints) {
      *error = "base_window_empty";
      *message = "翼面窗口 [" + mmText(mToMm(lo)) + ", " + mmText(mToMm(hi)) + "] mm 里只有 " +
                 std::to_string(window.size()) + " 个点";
      return false;
    }
    Eigen::VectorXf coef;
    pcl::Indices inliers;
    if (!gap_std::lineFit2D(window, &coef, &inliers, P.lineDistM) || coef.size() < 5) {
      *error = "surface_fit_failed";
      *message = "翼面拟合失败";
      return false;
    }
    Eigen::Vector2d d(coef[3], coef[4]);
    if (!(d.norm() > 0)) {
      *error = "surface_fit_failed";
      *message = "翼面拟合出的方向是零向量";
      return false;
    }
    d.normalize();
    if (d.x() < 0) d = -d;
    if (std::fabs(d.x()) < 1e-3) {
      *error = "invalid_geometry";
      *message = "翼面近乎竖直，定不了深度方向";
      return false;
    }
    line->p = Eigen::Vector2d(coef[0], coef[1]);
    line->d = d;
    line->n = Eigen::Vector2d(-d.y(), d.x());
    if (line->n.y() < 0) line->n = -line->n;
    line->inlierCount = inliers.size();
    double squareSum = 0.0;
    double worst = 0.0;
    std::size_t valid = 0;
    for (const auto index : inliers) {
      if (index < 0 || static_cast<std::size_t>(index) >= window.size()) continue;
      const auto& q = window[static_cast<std::size_t>(index)];
      const double residual = std::fabs(line->depth({q.x, q.y})) * kScale;
      squareSum += residual * residual;
      worst = std::max(worst, residual);
      ++valid;
    }
    if (valid > 0) {
      line->rmsMm = std::sqrt(squareSum / static_cast<double>(valid));
      line->maxMm = worst;
    }
    return true;
  };
  const auto fitBase = [&](double refX) {
    std::string error;
    std::string message;
    r.baseOk = fitFlank(refX, P.baseLeft, P.nearM, P.farM, &r.base, &error, &message);
    if (!r.baseOk) fail(r, error.c_str(), "基准" + message);
    return r.baseOk;
  };

  // 两把刀：基准侧圆边用平行于基准线、深 levelDepth 的刀切出 A 点 —— 圆边相对自己的翼面是刚性的，
  // A 点就是圆边上一个固定的位置；对面则用**过 A 点的水平刀**去切 —— 对面立边一旦沿基准面滑开 g，
  // 相对倾斜刀口的深度会变（k·g），相对水平刀口不会，读数才能严格加 g。
  const auto deepTilted = [&](std::size_t i) { return r.base.depth(pts[i]) > P.levelM; };
  const auto crossTilted = [&](std::size_t a, std::size_t b) {
    const double da = r.base.depth(pts[a]) - P.levelM;
    const double db = r.base.depth(pts[b]) - P.levelM;
    double t = (da - db) != 0.0 ? da / (da - db) : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return Sample{pts[a].x + t * (pts[b].x - pts[a].x), pts[a].y + t * (pts[b].y - pts[a].y)};
  };
  // 从 start 沿 dir 走，直到轮廓穿出刀口（连续 confirm 个浅点才算），返回插值出的穿出点。
  const auto walk = [&](std::ptrdiff_t start, int dir, const auto& deep, const auto& cross,
                        Sample* out) {
    auto i = start;
    auto lastDeep = i;
    while (true) {
      const auto j = i + dir;
      if (j < 0 || j >= n) return false;
      if (deep(static_cast<std::size_t>(j))) {
        i = j;
        lastDeep = j;
        continue;
      }
      bool confirmed = true;
      for (int c = 1; c < P.confirm; ++c) {
        const auto jj = j + dir * c;
        if (jj < 0 || jj >= n) break;
        if (deep(static_cast<std::size_t>(jj))) {
          confirmed = false;
          break;
        }
      }
      if (confirmed) {
        *out = cross(static_cast<std::size_t>(lastDeep), static_cast<std::size_t>(j));
        return true;
      }
      i = j;  // 孤立的浅点：噪声，跳过
    }
  };

  // 窗口以 A 点为参照迭代两三次：第一次只能拿圆边上的粗参照点，A 点定下来后再以它为准重拟，
  // 直到 A 点不再动。起点取参照点到锚点之间相对基准线最深的那个点（两圆边相碰的角），
  // 而不是锚点本身 —— 锚点若在对面立边脚下，缝张开后它相对倾斜刀口可能已经不够深。
  Sample edgeBase;
  double refX = pts[static_cast<std::size_t>(iRef)].x;
  double prevEdgeX = kNaN;
  std::ptrdiff_t iStart = static_cast<std::ptrdiff_t>(i0);
  r.iterations = 0;
  for (int iter = 0; iter < 4; ++iter) {
    ++r.iterations;
    if (!fitBase(refX)) return r;
    iStart = rangeLo;
    for (auto i = rangeLo; i <= rangeHi; ++i) {
      if (r.base.depth(pts[static_cast<std::size_t>(i)]) >
          r.base.depth(pts[static_cast<std::size_t>(iStart)])) {
        iStart = i;
      }
    }
    r.anchorDepth = r.base.depth(pts[static_cast<std::size_t>(iStart)]);
    if (!(r.anchorDepth > P.levelM)) {
      fail(r, "notch_too_shallow",
           "缝角只比基准翼面深 " + mmText(mToMm(r.anchorDepth)) + " mm，不到 levelDepth " +
               mmText(mToMm(P.levelM)) + " mm，这里没有 V 缝");
      return r;
    }
    if (!walk(iStart, baseDir, deepTilted, crossTilted, &edgeBase)) {
      fail(r, "notch_edge_not_found", "从缝角向基准侧走到框边，轮廓都没有穿出 levelDepth");
      return r;
    }
    if (std::isfinite(prevEdgeX) && std::fabs(edgeBase.x - prevEdgeX) < 0.00002) break;
    prevEdgeX = edgeBase.x;
    refX = edgeBase.x;
  }
  r.start = pts[static_cast<std::size_t>(iStart)];
  r.levelY = edgeBase.y;
  const auto deepFlat = [&](std::size_t i) { return pts[i].y > r.levelY; };
  const auto crossFlat = [&](std::size_t a, std::size_t b) {
    const double da = pts[a].y - r.levelY;
    const double db = pts[b].y - r.levelY;
    double t = (da - db) != 0.0 ? da / (da - db) : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return Sample{pts[a].x + t * (pts[b].x - pts[a].x), r.levelY};
  };
  Sample edgeOther;
  if (!walk(iStart, -baseDir, deepFlat, crossFlat, &edgeOther)) {
    fail(r, "notch_edge_not_found", "从缝角向对面走到框边，轮廓都没有升回 A 点的高度");
    return r;
  }
  r.left = P.baseLeft ? edgeBase : edgeOther;
  r.right = P.baseLeft ? edgeOther : edgeBase;
  r.width = r.right.x - r.left.x;
  r.valid = true;

  // 面差：另一侧的翼面线以 B 点为参照拟合，两条线都外推到缝心，垂距按基准线方向算
  //（同 gap.groove_joint）。拟不出来只标 refOk=false，不影响开口。
  {
    std::string error;
    std::string message;
    r.refOk = fitFlank(edgeOther.x, !P.baseLeft, P.refNearM, P.refFarM, &r.ref, &error, &message);
    if (!r.refOk) {
      r.refError = error + ": 对面" + message;
    } else {
      r.xMid = 0.5 * (r.left.x + r.right.x);
      const FlankLine& flushBase = (P.flushBaseLeft == P.baseLeft) ? r.base : r.ref;
      const FlankLine& flushRef = (P.flushBaseLeft == P.baseLeft) ? r.ref : r.base;
      const double cosB = 1.0 / std::hypot(1.0, flushBase.slope());
      r.flushRaw = (flushBase.yAt(r.xMid) - flushRef.yAt(r.xMid)) * cosB;
    }
  }
  return r;
}

nlohmann::json cameraJson(const CameraNotch& c) {
  nlohmann::json j;
  j["valid"] = c.valid;
  j["error"] = c.error;
  j["message"] = c.message;
  j["pointCount"] = c.pointCount;
  j["anchorMm"] = sampleMm(c.anchor);
  j["cornerMm"] = sampleMm(c.start);
  j["cornerDepthMm"] = numberOrNull(std::isfinite(c.anchorDepth) ? mToMm(c.anchorDepth) : kNaN);
  j["iterations"] = c.iterations;
  nlohmann::json base;
  base["pointCount"] = c.base.pointCount;
  base["inlierCount"] = c.base.inlierCount;
  base["windowMm"] = nlohmann::json::array({mToMm(c.base.lo), mToMm(c.base.hi)});
  base["slope"] = numberOrNull(c.baseOk ? c.base.slope() : kNaN);
  base["yAtAnchorMm"] =
      numberOrNull(c.baseOk && std::isfinite(c.anchor.x) ? mToMm(c.base.yAt(c.anchor.x)) : kNaN);
  base["rmsResidualMm"] = numberOrNull(c.base.rmsMm);
  base["maxResidualMm"] = numberOrNull(c.base.maxMm);
  j["base"] = std::move(base);
  j["levelYMm"] = numberOrNull(std::isfinite(c.levelY) ? mToMm(c.levelY) : kNaN);
  j["edgeLeftMm"] = sampleMm(c.left);
  j["edgeRightMm"] = sampleMm(c.right);
  j["widthMm"] = numberOrNull(std::isfinite(c.width) ? mToMm(c.width) : kNaN);
  nlohmann::json ref;
  ref["valid"] = c.refOk;
  ref["error"] = c.refError;
  ref["pointCount"] = c.ref.pointCount;
  ref["inlierCount"] = c.ref.inlierCount;
  ref["windowMm"] = nlohmann::json::array({mToMm(c.ref.lo), mToMm(c.ref.hi)});
  ref["slope"] = numberOrNull(c.refOk ? c.ref.slope() : kNaN);
  ref["yAtMidMm"] = numberOrNull(c.refOk && std::isfinite(c.xMid) ? mToMm(c.ref.yAt(c.xMid)) : kNaN);
  ref["rmsResidualMm"] = numberOrNull(c.ref.rmsMm);
  ref["maxResidualMm"] = numberOrNull(c.ref.maxMm);
  j["ref"] = std::move(ref);
  j["midXMm"] = numberOrNull(std::isfinite(c.xMid) ? mToMm(c.xMid) : kNaN);
  j["baseYAtMidMm"] =
      numberOrNull(c.baseOk && std::isfinite(c.xMid) ? mToMm(c.base.yAt(c.xMid)) : kNaN);
  j["flushRawMm"] = numberOrNull(std::isfinite(c.flushRaw) ? mToMm(c.flushRaw) : kNaN);
  return j;
}

nlohmann::json fitQualityJson(const FlankLine& f) {
  nlohmann::json q;
  q["model"] = "line";
  q["pointCount"] = f.pointCount;
  q["inlierCount"] = f.inlierCount;
  q["inlierRatio"] = f.pointCount == 0
                         ? nlohmann::json()
                         : nlohmann::json(static_cast<double>(f.inlierCount) / f.pointCount);
  q["linePointXMm"] = mToMm(f.p.x());
  q["linePointYMm"] = mToMm(f.p.y());
  q["lineDirX"] = f.d.x();
  q["lineDirY"] = f.d.y();
  q["rmsResidualMm"] = numberOrNull(f.rmsMm);
  q["maxResidualMm"] = numberOrNull(f.maxMm);
  return q;
}

lyflow::Line2D lineOf(const FlankLine& f) {
  lyflow::Line2D line;
  line.point[0] = static_cast<float>(f.p.x());
  line.point[1] = static_cast<float>(f.p.y());
  line.dir[0] = static_cast<float>(f.d.x());
  line.dir[1] = static_cast<float>(f.d.y());
  line.hasSegment = true;
  line.start[0] = static_cast<float>(f.lo);
  line.start[1] = static_cast<float>(f.yAt(f.lo));
  line.end[0] = static_cast<float>(f.hi);
  line.end[1] = static_cast<float>(f.yAt(f.hi));
  return line;
}

lyflow::Line2D segmentOf(const Sample& a, const Sample& b) {
  lyflow::Line2D seg;
  seg.hasSegment = true;
  seg.start[0] = static_cast<float>(a.x);
  seg.start[1] = static_cast<float>(a.y);
  seg.end[0] = static_cast<float>(b.x);
  seg.end[1] = static_cast<float>(b.y);
  seg.point[0] = seg.start[0];
  seg.point[1] = seg.start[1];
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double n = std::hypot(dx, dy);
  seg.dir[0] = n > 0 ? static_cast<float>(dx / n) : 1.0F;
  seg.dir[1] = n > 0 ? static_cast<float>(dy / n) : 0.0F;
  return seg;
}

Status notchWidth(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                  ExecContext& ctx) {
  const GapCloud primary = toPcl(*inputs.get("primary").asCloud());
  const GapCloud secondary = toPcl(*inputs.get("secondary").asCloud());

  NotchParams P;
  P.baseLeft = params.choice("baseSide") == "left";
  P.levelM = params.number("levelDepth") / kScale;
  P.deepestCount =
      static_cast<std::size_t>(std::max<std::int64_t>(1, params.integer("deepestCount")));
  P.nearM = params.number("baseNear") / kScale;
  P.farM = params.number("baseFar") / kScale;
  P.lineDistM = params.number("lineDistThresh") / kScale;
  P.minLinePoints =
      static_cast<std::size_t>(std::max<std::int64_t>(2, params.integer("minLinePoints")));
  P.confirm = static_cast<int>(std::max<std::int64_t>(1, params.integer("confirmPoints")));
  const std::string flushBase = params.choice("flushBase");
  P.flushBaseLeft = flushBase == "left" || (flushBase == "same" && P.baseLeft);
  P.refNearM = params.number("refNear") / kScale;
  P.refFarM = params.number("refFar") / kScale;
  const double scale = params.number("scale");
  const double gapOffset = params.number("gapOffset");
  const double flushOffset = params.number("flushOffset");
  const std::string camera = params.choice("camera");

  const CameraNotch cams[2] = {measureCamera(primary, P), measureCamera(secondary, P)};
  static const char* kCameraNames[2] = {"primary", "secondary"};

  std::vector<int> use;
  if (camera == "primary") {
    use = {0};
  } else if (camera == "secondary") {
    use = {1};
  } else {
    use = {0, 1};
  }
  double sum = 0.0;
  int validCount = 0;
  const CameraNotch* shown = nullptr;
  for (const int i : use) {
    if (!cams[i].valid) continue;
    sum += cams[i].width;
    ++validCount;
    if (shown == nullptr) shown = &cams[i];
  }
  if (validCount == 0) {
    const CameraNotch& first = cams[use.front()];
    std::string message;
    for (const int i : use) {
      if (!message.empty()) message += "；";
      message += std::string(kCameraNames[i]) + ": " + cams[i].message;
    }
    return Status::Error(Phase::Execute, first.error.empty() ? "notch_not_found" : first.error,
                         message, {}, kCameraNames[use.front()]);
  }
  const double widthRawM = sum / validCount;
  const double gapMm = mToMm(widthRawM) * scale + gapOffset;

  double flushSum = 0.0;
  int flushCount = 0;
  const CameraNotch* flushShown = nullptr;
  std::string flushError;
  for (const int i : use) {
    if (!cams[i].valid) continue;
    if (!cams[i].refOk) {
      if (!flushError.empty()) flushError += "；";
      flushError += std::string(kCameraNames[i]) + ": " + cams[i].refError;
      continue;
    }
    flushSum += cams[i].flushRaw;
    ++flushCount;
    if (flushShown == nullptr) flushShown = &cams[i];
  }
  const double flushRawMm = flushCount > 0 ? mToMm(flushSum / flushCount) : kNaN;
  const double flushMm = flushCount > 0 ? flushRawMm + flushOffset : kNaN;

  setMeasurement(outputs, "gap", gapMm, std::isfinite(gapMm), {});
  setMeasurement(outputs, "flush", flushMm, std::isfinite(flushMm),
                 flushCount > 0 ? std::string() : "对面翼面拟不出来，面差无效：" + flushError);
  outputs.set("baseLine", Data::line2d(lineOf(shown->base)));
  outputs.set("gapSegment", Data::line2d(segmentOf(shown->left, shown->right)));
  // 声明过的输出端口每一个都要写（executor 会检查），面差无效时写退化值。
  if (flushShown != nullptr) {
    outputs.set("refLine", Data::line2d(lineOf(flushShown->ref)));
    const Sample top{flushShown->xMid, flushShown->base.yAt(flushShown->xMid)};
    const Sample bottom{flushShown->xMid, flushShown->ref.yAt(flushShown->xMid)};
    outputs.set("flushSegment", Data::line2d(segmentOf(top, bottom)));
    lyflow::Record qualityRef;
    qualityRef.type = "GapFitQuality";
    qualityRef.data = fitQualityJson(flushShown->ref);
    outputs.set("qualityRef", Data::record(std::move(qualityRef)));
  } else {
    lyflow::Line2D empty;
    empty.point[0] = static_cast<float>(shown->anchor.x);
    empty.point[1] = static_cast<float>(shown->anchor.y);
    empty.hasSegment = false;
    outputs.set("refLine", Data::line2d(empty));
    outputs.set("flushSegment", Data::line2d(empty));
    lyflow::Record qualityRef;
    qualityRef.type = "GapFitQuality";
    qualityRef.data = fitQualityJson(FlankLine{});
    qualityRef.data["error"] = flushError;
    outputs.set("qualityRef", Data::record(std::move(qualityRef)));
  }
  lyflow::Point2D anchor;
  anchor.p[0] = static_cast<float>(shown->anchor.x);
  anchor.p[1] = static_cast<float>(shown->anchor.y);
  outputs.set("anchor", Data::point2d(anchor));

  lyflow::Record quality;
  quality.type = "GapNotchQuality";
  quality.data["baseSide"] = P.baseLeft ? "left" : "right";
  quality.data["camera"] = camera;
  quality.data["levelDepthMm"] = params.number("levelDepth");
  quality.data["widthRawMm"] = mToMm(widthRawM);
  quality.data["validCameras"] = validCount;
  quality.data["flushBase"] = P.flushBaseLeft ? "left" : "right";
  quality.data["flushRawMm"] = numberOrNull(flushRawMm);
  quality.data["flushCameras"] = flushCount;
  quality.data["scale"] = scale;
  for (int i = 0; i < 2; ++i) quality.data["cameras"][kCameraNames[i]] = cameraJson(cams[i]);
  outputs.set("quality", Data::record(std::move(quality)));

  lyflow::Record qualityBase;
  qualityBase.type = "GapFitQuality";
  qualityBase.data = fitQualityJson(shown->base);
  outputs.set("qualityBase", Data::record(std::move(qualityBase)));

  ctx.log(LogLevel::Info, "notch: 锚点 x " + mmText(mToMm(shown->anchor.x)) + " mm，两侧穿出 x " +
                              mmText(mToMm(shown->left.x)) + " / " + mmText(mToMm(shown->right.x)) +
                              " mm，开口 " + mmText(gapMm) + " mm，面差 " +
                              (flushCount > 0 ? mmText(flushMm) : std::string("无效")) + " mm（" +
                              std::to_string(validCount) + " 台相机）");
  return Status::Ok();
}

Param numParam(const char* name, const char* label, double def, const char* unit,
               const char* group, const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Float;
  p.label = label;
  p.doc = doc;
  p.def = Value::number(def);
  p.unit = unit;
  p.group = group;
  return p;
}

Param intParam(const char* name, const char* label, std::int64_t def, const char* group,
               const char* doc) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = label;
  p.doc = doc;
  p.def = Value::integer(def);
  p.group = group;
  return p;
}

}  // namespace

void registerNotchWidth(Registry& r) {
  OperatorDesc op;
  op.id = "gap.notch_width";
  op.version = "1.0.0";
  op.label = "V 缝开口";
  op.category = "间隙/测量";
  op.keywords = {"notch", "V", "开口", "夹角", "软装", "间隙", "圆边"};
  op.doc =
      "两件软装的圆边贴合成 V 形缝、缝底没有槽时的间隙（罗石 点 1 / 1_1）。\n"
      "以基准侧翼面（锚点一侧 [baseNear, baseFar] 窗口里拟的直线）为深度基准，在它下方 "
      "levelDepth 处切一刀，从最深点向两侧沿轮廓走到穿出这一刀的位置（线性插值），"
      "两个穿出点的水平距离就是开口宽度。\n"
      "**每台相机分开算再平均**。被另一件遮住缝底的相机看不到对面的立边，只会量出自己的阴影 —— "
      "这种测点请用 camera 只选看得见缝底的那一台。\n"
      "面差 = 对面翼面线（以 B 点为参照的 [refNear, refFar] 窗口）与基准线都外推到缝心后的垂距，"
      "flushBase 定符号。\n"
      "缝闭合时读数是两侧圆边在该深度的固有宽度（点 1 约 1.4 mm），用 gapOffset 归零；"
      "缝张开 g，读数就加 g。levelDepth 要小于闭合时缝角到基准线的深度（quality 里的 "
      "cornerDepthMm），否则报 notch_too_shallow。";
  op.preconditions = {
      "levelDepth 必须落在立边高度内：要小于闭合时缝角到基准翼面线的深度（quality 的 "
      "cornerDepthMm），否则报 notch_too_shallow。",
      "缓坡末端没有圆边下凹的缝不适用 —— 那里的最深点不是缝角，切一刀也穿不出去。",
      "读数是「两侧圆边在该深度的固有宽度 + 张开量」，不是绝对间隙；要先在闭合件上用 "
      "gapOffset 归零才有意义。",
  };
  op.inputs = {
      Port{"primary", "PointCloud", "Primary", "整体 ROI 裁过的 Master 云（测量帧）。", true},
      Port{"secondary", "PointCloud", "Secondary", "整体 ROI 裁过的 Slave 云（测量帧）。", true},
  };
  op.outputs = {
      Port{"gap", "Measurement", "Gap", "开口宽度，毫米。", true},
      Port{"flush", "Measurement", "Flush",
           "面差，毫米：非 flushBase 那一侧翼面线相对 flushBase 侧翼面线在缝心处的垂距，+ = 更高。"
           "对面翼面拟不出来时 ok=false，不影响开口。",
           true},
      Port{"baseLine", "Line2D", "Base Line", "基准翼面直线（带窗口两端），取自第一台有效相机。",
           true},
      Port{"refLine", "Line2D", "Ref Line", "对面翼面直线（带窗口两端）。面差无效时不输出。", false},
      Port{"gapSegment", "Line2D", "Gap Segment", "两个穿出点之间的那一段。", true},
      Port{"flushSegment", "Line2D", "Flush Segment", "缝心处基准线到对面线的那一段。面差无效时不输出。",
           false},
      Port{"anchor", "Point2D", "Anchor", "锚点：最深的那几个点里靠中间的一个。", true},
      withExample(Port{"quality", "Record", "Quality",
                       "GapNotchQuality：逐相机的锚点、基准线、穿出点与宽度。", true},
                  examples::notchQuality()),
      withExample(Port{"qualityBase", "Record", "Quality Base",
                       "GapFitQuality：基准翼面窗口的拟合残差。", true},
                  examples::fitQuality()),
      withExample(Port{"qualityRef", "Record", "Quality Ref",
                       "GapFitQuality：对面翼面窗口的拟合残差。面差无效时不输出。", false},
                  examples::fitQuality()),
  };

  Param flushBase;
  flushBase.name = "flushBase";
  flushBase.type = ParamType::Enum;
  flushBase.label = "Flush Base";
  flushBase.doc =
      "面差以哪一侧为基准：flush = 另一侧相对它的高度，+ = 另一侧更高。same 跟随 baseSide。"
      "baseSide 决定开口的深度基准（取平翼面那侧），flushBase 决定面差的符号，两者可以不同 ——"
      "罗石 点 2 以左侧面板翼面定深度、以右侧为面差基准，面板低读负。";
  flushBase.def = Value::text("same");
  flushBase.options = {EnumOption{"same", "Same（同 baseSide）", ""},
                       EnumOption{"left", "Left（左侧为面差基准）", ""},
                       EnumOption{"right", "Right（右侧为面差基准）", ""}};

  Param baseSide;
  baseSide.name = "baseSide";
  baseSide.type = ParamType::Enum;
  baseSide.label = "Base Side";
  baseSide.doc = "平的那一侧翼面在缝的哪一边，深度以它为准。";
  baseSide.def = Value::text("left");
  baseSide.options = {EnumOption{"left", "Left（左侧为基准）", ""},
                      EnumOption{"right", "Right（右侧为基准）", ""}};

  Param camera;
  camera.name = "camera";
  camera.type = ParamType::Enum;
  camera.label = "Camera";
  camera.doc =
      "用哪几台相机。both 是有效相机的平均；被遮挡看不到缝底的那一台会量出自己的阴影，"
      "这时只选看得见的那台。";
  camera.def = Value::text("both");
  camera.options = {EnumOption{"both", "Both（有效相机平均）", ""},
                    EnumOption{"primary", "Primary（Master）", ""},
                    EnumOption{"secondary", "Secondary（Slave）", ""}};

  Param scale;
  scale.name = "scale";
  scale.type = ParamType::Float;
  scale.label = "Scale";
  scale.doc = "读数增益：gap = 开口宽度 × scale + gapOffset。";
  scale.def = Value::number(1.0);

  op.params = {
      baseSide,
      camera,
      flushBase,
      numParam("levelDepth", "Level Depth", 0.25, "mm", "开口",
               "在基准翼面线下方多深处切基准侧圆边。要小于闭合时缝角的深度（cornerDepthMm）。"),
      intParam("deepestCount", "Deepest Count", 3, "锚点",
               "取最深的几个点定锚点，x 取它们的中位数。"),
      numParam("baseNear", "Base Near", 2.0, "mm", "基准翼面",
               "拟合窗口的内边：离锚点这么远才开始取点，避开圆边。"),
      numParam("baseFar", "Base Far", 7.0, "mm", "基准翼面", "拟合窗口的外边。"),
      numParam("lineDistThresh", "Line Dist Thresh", 0.1, "mm", "基准翼面",
               "直线拟合的内点判定距离。单台相机的点噪声约 0.02 mm。"),
      intParam("minLinePoints", "Min Line Points", 8, "基准翼面",
               "窗口里少于这个点数，这台相机整个不参与。"),
      intParam("confirmPoints", "Confirm Points", 2, "开口",
               "连续几个浅点才算真的穿出了刀口；中间孤立的浅点当噪声跳过。"),
      scale,
      numParam("gapOffset", "Gap Offset", 0.0, "mm", "开口",
               "加在开口宽度上的偏置，用它让闭合缝读 0。"),
      numParam("refNear", "Ref Near", 2.5, "mm", "面差",
               "对面翼面窗口的内边：离 B 点这么远才开始取点，避开圆边/立边。"),
      numParam("refFar", "Ref Far", 7.0, "mm", "面差", "对面翼面窗口的外边。"),
      numParam("flushOffset", "Flush Offset", 0.0, "mm", "面差", "加在面差上的偏置。"),
  };
  op.capabilities = {false, true, true};
  op.compute = &notchWidth;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::packs::gap
