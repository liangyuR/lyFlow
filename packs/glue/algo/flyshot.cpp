#include "algo/flyshot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include <opencv2/imgproc.hpp>

namespace lyflow::packs::glue {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
/// 峰离搜索窗边 ≤ 1 px 判贴边，所以示教框至少要离图边这么远（px）。
constexpr double kBorderMin = 2.0;
/// 匹配前对搜索区和模板做同样的高斯平滑（σ，px）。每个角度（含 0°）都经同一次三次插值旋转，但插值在非零角度上
/// 仍相当于再平滑一点、在 0° 上（不挪亚像素相位时）是原样拷贝；白噪声下平滑过的那几个角度 NCC 反而更高，噪声会把角度推离 0
/// （σ = 6 的纯噪声帧：220 px 模板 0.25°，2026-10-17 样本集 96 px 模板中位 0.63°）。先平滑掉单像素噪声后插值几乎
/// 不再改变图像，同样的帧上降到 ≤ 0.004° / P95 0.06°（双线性 + 预平滑仍有 0.46°，所以用三次插值）。
/// 平移的亚像素精度不受影响：两边平滑一样，峰不偏 —— 模板带足外圈（templatePad ≥ kLocateSmoothRadius）时
/// 严格一样；不带外圈时框外紧挨着有强边会偏（见下面预平滑处）。
constexpr double kPreSmoothSigma = 1.0;
/// 看不看得出角度：粗扫各角度（含护栏）里最高分与最低分之差，要超过这一帧本身的失配（1 − 最高分）的
/// kAngleContrastGain 倍，夹在 [kAngleContrastMin, kAngleContrastMax] 里；看不出时角度报 0、angleObservable = false。
/// 预平滑之后圆孔模板的差只剩 0.00015 左右（之前 ≤ 0.0036，是插值造出来的）。下限 0.004 现在拦的是「真实模板
/// 角度约束太弱」：2026-10-17 样本集 36 个选中的 96 px 模板里有 5 个差 0.001…0.004（转不转、加不加噪声都是），
/// 整图转 ±1…2° 时照估的角度误差最大 0.14…0.67°；三对复拍（真实转角≈0）里两对差 0.0022 / 0.0037，照估会报出
/// 0.25° 的假角度，只找平移反而更准。别的模板转 ±2° 时差 0.005…0.06。（这些数是按亚像素相位对齐、按插值峰顶比分
/// 之前量的；之后三对复拍里 S002→S035 判看得出、报 0.15°，见 docs/glue-integration.md「亚像素相位」一节。）
/// 差值过了上限总算看得出：转角远超范围时分数掉得多、失配也大，不能让它按比例躲过 at_angle_edge。
constexpr double kAngleContrastGain = 4.0;
constexpr double kAngleContrastMin = 0.004;
constexpr double kAngleContrastMax = 0.02;
/// 看不出角度、最高分却落在护栏上时，比 0° 高出看不看得出角度那道门（上面的差值门限）的这个比例才判 at_angle_edge：
/// 下限 0.004 时就是 0.0005，是预平滑后圆孔模板各角度分数差（0.00015 左右）的三倍多，圆孔不会因为最高分恰好
/// 落在护栏上被判贴边。样本集角度约束弱的模板（S011 / S019 / S028）整图转 +4° 时最高分比 0° 高 0.0007…0.0027。
constexpr double kOffScanFraction = 0.125;
/// peakRatio 只认突起度不低于它的峰：噪声在宽峰侧坡上起的小疙瘩不算第二个峰。
constexpr double kPeakProminenceMin = 0.02;

/// 三点抛物线的顶点相对中点的偏移（−0.5 … 0.5）。不是尖峰（开口朝上或平）时返回 0。
double parabolaPeak(double a, double b, double c) {
  const double den = a - 2.0 * b + c;
  if (!(den < 0.0))
    return 0.0;
  return std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

/// 过 (−1, a)、(0, b)、(1, c) 的抛物线在 t 处比 b 高出多少。
double parabolaRise(double a, double b, double c, double t) {
  return 0.5 * (c - a) * t + 0.5 * (a - 2.0 * b + c) * t * t;
}

/// 双线性采样；落出图外返回 NaN。
double sample(const cv::Mat& g, double x, double y) {
  if (!(x >= 0.0 && y >= 0.0 && x <= g.cols - 1.0 && y <= g.rows - 1.0))
    return kNaN;
  const int x0 = std::min(static_cast<int>(x), g.cols - 2);
  const int y0 = std::min(static_cast<int>(y), g.rows - 2);
  const double ax = x - x0, ay = y - y0;
  const uchar* r0 = g.ptr<uchar>(y0) + x0;
  const uchar* r1 = g.ptr<uchar>(y0 + 1) + x0;
  const double top = r0[0] + ax * (r0[1] - r0[0]);
  const double bottom = r1[0] + ax * (r1[1] - r1[0]);
  return top + ay * (bottom - top);
}

struct Peak {
  int x = 0, y = 0;
  double value = 0;
};

/// 响应图里突起度 ≥ minProminence 的局部极大（8 邻域；整张图最高的那个突起度算无穷大，总在里面）。
/// 按值从高到低把像素逐个加进来、用并查集合并相邻的块：两块相遇时峰较低的那块到此为止，
/// 它的突起度 = 峰顶 − 相遇处的值。
std::vector<Peak> prominentPeaks(const cv::Mat& response, double minProminence) {
  const int w = response.cols, h = response.rows, count = w * h;
  std::vector<float> v(static_cast<std::size_t>(count));
  for (int y = 0; y < h; ++y) {
    const float* row = response.ptr<float>(y);
    for (int x = 0; x < w; ++x) // NaN 会打乱排序，当成最低
      v[static_cast<std::size_t>(y) * w + x] = std::isfinite(row[x]) ? row[x] : -1.0f;
  }
  std::vector<int> order(static_cast<std::size_t>(count));
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return v[a] > v[b] || (v[a] == v[b] && a < b); });
  std::vector<int> parent(static_cast<std::size_t>(count), -1); // -1 = 还没加进来
  std::vector<int> top(static_cast<std::size_t>(count));         // 块的根上记这块的峰顶
  auto find = [&](int i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  std::vector<Peak> peaks;
  for (const int i : order) {
    parent[i] = top[i] = i;
    const int x = i % w, y = i / w;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = x + dx, ny = y + dy;
        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h ||
            parent[ny * w + nx] < 0)
          continue;
        int a = find(i), b = find(ny * w + nx);
        if (a == b)
          continue;
        if (v[top[a]] < v[top[b]] || (v[top[a]] == v[top[b]] && top[a] > top[b]))
          std::swap(a, b);
        if (v[top[b]] - v[i] >= minProminence)
          peaks.push_back(Peak{top[b] % w, top[b] / w, v[top[b]]});
        parent[b] = a;
      }
    }
  }
  for (int i = 0; i < count; ++i) {
    if (parent[i] == i)
      peaks.push_back(Peak{top[i] % w, top[i] / w, v[top[i]]});
  }
  return peaks;
}

} // namespace

P2 Pose2D::rotate(P2 v) const {
  const double a = angleDeg * CV_PI / 180.0;
  const double c = std::cos(a), s = std::sin(a);
  return P2(c * v.x - s * v.y, s * v.x + c * v.y);
}

P2 Pose2D::apply(P2 p) const { return rotate(p - teachCenter) + center; }

Pose2D locateTemplate(const cv::Mat& gray, const cv::Mat& tmpl, const LocateSpec& spec,
                      std::string* why) {
  Pose2D pose;
  auto fail = [&](const char* code, const char* m) {
    pose.ok = false;
    pose.reason = code;
    if (why)
      *why = m;
    return pose;
  };
  if (gray.empty() || tmpl.empty() || gray.type() != CV_8UC1 || tmpl.type() != CV_8UC1) {
    // 算子在进来之前已经把这种输入拒成执行错误；直接调内核时没有图可搜，归到 out_of_image
    return fail("out_of_image", "图像和模板需要非空 u8 灰度图");
  }
  // tmpl 带 templatePad 宽的外圈时，示教框是去掉外圈的那一块；下面除了预平滑，全都只看示教框
  const int tp = spec.templatePad;
  if (tp < 0 || tmpl.cols <= 2 * tp || tmpl.rows <= 2 * tp)
    return fail("out_of_image", "模板图不比两倍 templatePad 大，去掉外圈后没有示教框");
  const cv::Rect innerBox(tp, tp, tmpl.cols - 2 * tp, tmpl.rows - 2 * tp);
  const cv::Mat inner = tmpl(innerBox);
  const int tw = inner.cols, th = inner.rows;
  pose.teachCenter = spec.anchor + P2((tw - 1) / 2.0, (th - 1) / 2.0);
  pose.center = pose.teachCenter;
  cv::Scalar mean, deviation;
  cv::meanStdDev(inner, mean, deviation);
  // OpenCV 对常量模板的 TM_CCOEFF_NORMED 返回 1；没有纹理不能作为成功定位的证据。
  if (deviation[0] < 1.0)
    return fail("flat_template", "模板对比度不足，请选择含边缘或纹理的模板");
  // 示教框离图边不到 2 px 时，零位移的峰就落在搜索窗边的贴边带里，与「真值在图外、峰被图边挡住」分不开，
  // 贴边门会让它永远失败；直接说清楚是示教框放得不对
  if (spec.anchor.x < kBorderMin || spec.anchor.y < kBorderMin ||
      spec.anchor.x + tw > gray.cols - kBorderMin || spec.anchor.y + th > gray.rows - kBorderMin) {
    return fail("out_of_image", "模板示教框离图边不到 2 px：零件往外一动模板就出图，请示教时离图边留出余量");
  }
  // 带外圈的模板（示教框四边各外扩 templatePad）是从同样大小的示教图上裁的，放不进这一帧说明示教框离图边
  // 不到 templatePad：外圈裁不全（或帧与示教图不是同一个相机画幅），同样判 out_of_image
  if (spec.anchor.x < tp || spec.anchor.y < tp || spec.anchor.x + tw + tp > gray.cols ||
      spec.anchor.y + th + tp > gray.rows) {
    return fail("out_of_image",
                "带外圈的模板（示教框四边各外扩 templatePad px）放不进这一帧：示教框离图边不到 templatePad，"
                "请示教时离图边留出余量");
  }
  // 模板旋转后外接圆的半径 + 搜索半径：任何允许的位姿下模板都完整落在这一块里
  const double half = spec.searchRadius + 0.5 * std::hypot(tw, th) + 4.0;
  const cv::Rect want(static_cast<int>(std::floor(pose.teachCenter.x - half)),
                      static_cast<int>(std::floor(pose.teachCenter.y - half)),
                      static_cast<int>(std::ceil(2.0 * half)) + 1,
                      static_cast<int>(std::ceil(2.0 * half)) + 1);
  const cv::Rect roi = want & cv::Rect(0, 0, gray.cols, gray.rows);
  if (roi.width < tw + 2 || roi.height < th + 2)
    return fail("out_of_image", "搜索区落在图外，或者比模板还小");
  const P2 rc((roi.width - 1) / 2.0, (roi.height - 1) / 2.0);
  // 搜索区和模板做同样的预平滑（见 kPreSmoothSigma），在浮点上做，不再量化回 u8（量化误差又是白噪声）。
  // 核半径 pad = 4σ（kLocateSmoothRadius）。搜索区按外扩 pad 的一块平滑再裁回来，边上用的是真实邻域。
  // 模板连外圈（templatePad）整块平滑、再裁回示教框，示教框不裁边：以前平滑后四边各裁 4 px，贴着框边的特征
  // （离框边 < 4 px 的细条、角点）被整个裁掉，只剩平滑的尾巴，平移漂 1…2 px 还照常成功。
  // templatePad ≥ pad 时示教框里每个像素平滑用的都是真实邻域，与图上同一处平滑出来的一样，框外的强边不偏峰。
  // 外圈不够（templatePad = 0 的旧图）时缺的邻域按 tmpl 边上的像素延伸（BORDER_REPLICATE）：框外真实邻域与框边
  // 一样时（框边落在平坦处，或特征垂直穿过框边）没有差别；框外 1.5 px 内有平行于框边的强边（暗半平面）时，
  // 模板框边上少了那条边的平滑尾巴、图上有，平移被拉偏 —— 合成图上离框边 0 / 0.5 / 1 / 1.5 px 时偏
  // 0.61 / 0.33 / 0.15 / 0.06 px（扫 ±3° 时 0.65 / 0.38 / 0.22 / 0.10 px），分数仍 > 0.96、照常成功
  const int pad = static_cast<int>(std::ceil(4.0 * kPreSmoothSigma));
  static_assert(4.0 * kPreSmoothSigma <= kLocateSmoothRadius &&
                    4.0 * kPreSmoothSigma > kLocateSmoothRadius - 1,
                "kLocateSmoothRadius 要等于 ⌈4σ⌉");
  const cv::Size kernel(2 * pad + 1, 2 * pad + 1);
  cv::Mat sub, smoothTmpl;
  {
    const cv::Rect padded =
        cv::Rect(roi.x - pad, roi.y - pad, roi.width + 2 * pad, roi.height + 2 * pad) &
        cv::Rect(0, 0, gray.cols, gray.rows);
    cv::Mat f;
    gray(padded).convertTo(f, CV_32F);
    cv::GaussianBlur(f, f, kernel, kPreSmoothSigma, kPreSmoothSigma, cv::BORDER_REPLICATE);
    sub = f(roi - padded.tl()).clone();
    cv::Mat t;
    tmpl.convertTo(t, CV_32F);
    cv::GaussianBlur(t, t, kernel, kPreSmoothSigma, kPreSmoothSigma, cv::BORDER_REPLICATE);
    smoothTmpl = t(innerBox).clone();
  }
  const int cw = smoothTmpl.cols, ch = smoothTmpl.rows;
  const P2 halfT((cw - 1) / 2.0, (ch - 1) / 2.0);

  /// 一个角度上的匹配：搜索窗（转过的图里、按模板左上角计）里的 NCC 响应与其中的最高点。
  struct Match {
    double phi = 0;
    double score = 0;  ///< 最高点（整像素）的 NCC
    double peak = 0;   ///< 抛物线插值到亚像素峰顶的 NCC：比各角度时用它，不受峰落在整像素哪一侧影响
    P2 loc;            ///< 最高点，含亚像素插值（转过的图里模板左上角）
    bool atEdge = true; ///< 最高点离搜索窗边 ≤ 1 px；没有搜索窗也算
    cv::Mat window;    ///< 搜索窗里的响应
  };

  // 把搜索区转 phi 度（绕它的中心），在转过的图里做 NCC：物体在原图里就是转了 −phi。
  // NCC 只在整像素上取分：物体落在半像素上时峰值被压低，压低多少随角度变（转过去的位置的小数部分随角度变），
  // 各角度的分数里就混进了与转角无关的亚像素相位，最好的角度被推到恰好对齐整像素的那个（无噪声、只平移半像素、
  // 不转的帧报 0.25…0.44°，复拍 S001→S034 报 0.49°）。两处一起去掉它：
  // - expect（模板左上角的估计，搜索区坐标）给了时，把转过的图再挪一个亚像素量，让这个位置转过去后落在整像素上，
  //   每个角度（含 0°）都在同一个相位上比，也都经同一种插值；
  // - 比各角度时用抛物线插值到亚像素峰顶的值（Match::peak），不用整像素上的值。
  // 只用一样不够：只对齐相位时合成图上 220 px 模板转 1.5° 差 0.11°（估计位置的那点误差在峰尖的角度上扣分更多），
  // 只用插值峰顶时半像素帧仍报 0.12°
  auto matchAt = [&](double phiDeg, const P2* expect) {
    Match m;
    m.phi = phiDeg;
    const double a = phiDeg * CV_PI / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    P2 shift(0, 0);
    if (expect) {
      const P2 e0 = *expect + halfT - rc;
      const P2 e(c * e0.x - s * e0.y + rc.x - halfT.x, s * e0.x + c * e0.y + rc.y - halfT.y);
      shift = P2(std::round(e.x) - e.x, std::round(e.y) - e.y);
    }
    // 只认模板中心离示教位置不超过 searchRadius 的匹配（转过的图里，示教中心也跟着转）。
    // 响应图（整块搜索区上的）里只有这一窗有用，旋转与 NCC 都只算盖住它的那一块
    const P2 tcRoi = pose.teachCenter - P2(roi.x, roi.y);
    const P2 d = tcRoi - rc;
    const P2 want(c * d.x - s * d.y + rc.x - (cw - 1) / 2.0,
                  s * d.x + c * d.y + rc.y - (ch - 1) / 2.0);
    const int fullCols = sub.cols - cw + 1, fullRows = sub.rows - ch + 1;
    const int u0 = std::max(0, static_cast<int>(std::floor(want.x - spec.searchRadius)));
    const int v0 = std::max(0, static_cast<int>(std::floor(want.y - spec.searchRadius)));
    const int u1 = std::min(fullCols - 1, static_cast<int>(std::ceil(want.x + spec.searchRadius)));
    const int v1 = std::min(fullRows - 1, static_cast<int>(std::ceil(want.y + spec.searchRadius)));
    if (u1 < u0 || v1 < v0) {
      m.loc = want;
      return m;
    }
    // 0° 也走同一次 warpAffine，不给 0° 单独开一条路
    const cv::Matx23d M(c, -s, rc.x - c * rc.x + s * rc.y - u0 + shift.x, s, c,
                        rc.y - s * rc.x - c * rc.y - v0 + shift.y);
    cv::Mat rot;
    cv::warpAffine(sub, rot, M, cv::Size(u1 - u0 + cw, v1 - v0 + ch), cv::INTER_CUBIC,
                   cv::BORDER_REPLICATE);
    cv::matchTemplate(rot, smoothTmpl, m.window, cv::TM_CCOEFF_NORMED);
    const cv::Mat& res = m.window;
    double best = 0;
    cv::Point at;
    cv::minMaxLoc(res, nullptr, &best, nullptr, &at);
    double dx = 0, dy = 0;
    if (at.x > 0 && at.x + 1 < res.cols) {
      dx = parabolaPeak(res.at<float>(at.y, at.x - 1), best, res.at<float>(at.y, at.x + 1));
    }
    if (at.y > 0 && at.y + 1 < res.rows) {
      dy = parabolaPeak(res.at<float>(at.y - 1, at.x), best, res.at<float>(at.y + 1, at.x));
    }
    // 搜索窗被图边裁过时，裁出来的边同样是边：模板再往外就出图了
    m.atEdge = at.x <= 1 || at.y <= 1 || at.x >= res.cols - 2 || at.y >= res.rows - 2;
    m.loc = P2(u0 + at.x + dx - shift.x, v0 + at.y + dy - shift.y);
    m.score = std::isfinite(best) ? best : 0.0;
    m.peak = m.score;
    if (at.x > 0 && at.x + 1 < res.cols)
      m.peak += parabolaRise(res.at<float>(at.y, at.x - 1), best, res.at<float>(at.y, at.x + 1), dx);
    if (at.y > 0 && at.y + 1 < res.rows)
      m.peak += parabolaRise(res.at<float>(at.y - 1, at.x), best, res.at<float>(at.y + 1, at.x), dy);
    return m;
  };

  // angleRange 比 angleStep 还小时范围内按 angleRange 一步扫：不然一个角度都不扫，也就判不了 at_angle_edge，
  // 却报 angleObservable = true（宿主要了转角补偿，拿到的 0° 会被当成「零件没转」）
  const double angleStep = std::max(spec.angleStep, 0.01);
  const double step = spec.angleRange > 0 ? std::min(angleStep, spec.angleRange) : angleStep;
  const int n =
      spec.angleRange > 0 ? static_cast<int>(std::floor(spec.angleRange / step + 1e-9)) : 0;
  // 扫角度时两端各多扫一步（护栏）：真实转角贴着 ±angleRange 时最好的粗扫角度落在端点很正常，
  // 不能凭端点判贴边；细扫后的角度出了 ±angleRange 才算（见下面的 at_angle_edge）。
  // 护栏在 ±(n·step + angleStep)，总比范围端点多出一个 angleStep：angleRange ≥ angleStep 时就是 ±(n + 1)·step；
  // angleRange 更小时也不跟着缩。看不看得出角度的门限是绝对的（kAngleContrastMin），扫的范围跟着缩到 ±2·angleRange 时，
  // 角度约束很强的模板分数也差不出 0.004（220 px 合成模板 angleRange = 0.1° 时判看不出，转 ±1.1…2° 照报成功、中心偏 1.2…2.6 px）
  const int g = n > 0 ? n + 1 : 0;
  auto coarseAngle = [&](int i) {
    if (std::abs(i) <= n)
      return i * step;
    return (i > 0 ? 1.0 : -1.0) * (n * step + angleStep);
  };
  // 扫角度前先在 0° 上找一次平移，粗扫的各角度都按它对齐亚像素相位（见 matchAt）；不扫角度时不需要
  P2 p0;
  const P2* expect = nullptr;
  if (n > 0) {
    p0 = matchAt(0.0, nullptr).loc;
    expect = &p0;
  }
  // 转过 phiDeg 的图里的点 q 换回搜索区坐标（绕搜索区中心转）
  auto unrotate = [&](double phiDeg, P2 q) {
    Pose2D back;
    back.angleDeg = -phiDeg;
    return back.rotate(q - rc) + rc;
  };
  std::vector<double> scores(2 * g + 1);
  std::vector<P2> locs(2 * g + 1);
  for (int i = -g; i <= g; ++i) {
    const Match m = matchAt(coarseAngle(i), expect);
    scores[i + g] = m.peak;
    locs[i + g] = m.loc;
  }
  int bestI = g; // 分数一样时取 0°
  for (int i = 0; i <= 2 * g; ++i) {
    if (scores[i] > scores[bestI])
      bestI = i;
  }
  const double lowest = *std::min_element(scores.begin(), scores.end());
  double phi = coarseAngle(bestI - g);
  bool angleOff = false; // 细扫后的角度出了 ±angleRange，或看不出角度但分数朝扫描范围外升上去
  if (n > 0) {
    // 细扫与最终匹配改按粗扫最好的角度上的位置对齐相位：零件转了时 0° 上的位置估得偏一点，各角度的失配
    // 一样大也会偏向峰钝的角度（峰越尖、同样的失配扣分越多），离真实角度最近的粗扫角度上的位置更准
    p0 = unrotate(phi, locs[bestI] + halfT) - halfT;
    // 粗扫的峰附近再按 step/4 细扫五个角度（k = −2…2），取最好的那个和左右两个做抛物线。
    // 中间那个也按新的 p0 重算：粗扫的分数是按 0° 上的位置对齐的相位，与另外四个混着比会偏 bestK 和抛物线（≤ 0.02°）。
    // 最好的落在窗口两端（k = ±2）时往外再补一个（k = ±3）再做抛物线：真实转角离粗扫角度 0.375…0.5 步时
    // 不补就停在 ±2 上，角度量化成 ±0.5 步（默认步长时整整差 0.05°）
    const double fine = step / 4.0;
    const double best = scores[bestI];
    double fineScores[7] = {0, 0, 0, 0, 0, 0, 0}; // k = −3…3
    int bestK = 3;
    for (int k = -2; k <= 2; ++k)
      fineScores[k + 3] = matchAt(phi + k * fine, expect).peak;
    for (int i = 1; i <= 5; ++i) { // 分数一样时取中间、再取靠前的
      if (fineScores[i] > fineScores[bestK])
        bestK = i;
    }
    if (bestK == 1 || bestK == 5) {
      const int k = bestK == 1 ? -3 : 3;
      fineScores[k + 3] = matchAt(phi + k * fine, expect).peak;
    }
    phi += (bestK - 3) * fine;
    phi += fine * parabolaPeak(fineScores[bestK - 1], fineScores[bestK], fineScores[bestK + 1]);
    // 圆孔这类旋转对称的模板、或角度约束太弱的模板看不出角度：各角度分数几乎一样，挑出来的角度是随机的。
    // 不报这个角度：角度报 0、angleObservable = false，位置取 0° 上最好的平移。
    // 例外是最高分落在护栏上（±(n·step + angleStep)，已出 ±angleRange）、又比 0° 高出一截（见 kOffScanFraction）：
    // 分数是朝扫描范围外一路升上去的，不是平的，真实转角多半在范围外，0° 上的平移也跟着偏（样本集 +4° 时偏 7…13 px），
    // 判 at_angle_edge（angle 报细扫出的角度）
    const double contrastMin = std::clamp(kAngleContrastGain * (1.0 - best), kAngleContrastMin,
                                          kAngleContrastMax);
    const bool observable = best - lowest >= contrastMin;
    if (!observable) {
      pose.angleObservable = false;
      const bool onGuard = bestI == 0 || bestI == 2 * g;
      if (onGuard && best - scores[g] >= kOffScanFraction * contrastMin) {
        angleOff = true;
      } else {
        // 只补平移：与 angleRange = 0 走同一条路（0° 不挪相位，就是原图），位置与只找平移的结果一致
        phi = 0.0;
        expect = nullptr;
      }
    } else if (std::fabs(phi) > spec.angleRange) {
      angleOff = true;
    }
  }
  const Match final = matchAt(phi, expect);
  pose.score = final.score;
  pose.center = unrotate(phi, final.loc + halfT) + P2(roi.x, roi.y);
  pose.angleDeg = -phi;

  // peakRatio：最终角度的搜索窗里，主峰之外最高的「独立峰」/ 主峰分数。
  // 独立峰 = 突起度（峰顶减去通往更高峰的路上必经的最低点）≥ kPeakProminenceMin 的局部极大。
  // 真实图上峰很宽，侧坡离开主峰几十 px 仍有 0.95 以上，按固定半径排除会把侧坡当成第二个峰；
  // 模板里有大片明暗渐变时整个窗口都是 0.9 左右的高台，按「主峰半高连通」排除又会把高台上的第二个孔一起吞掉。
  // 突起度两头都不受影响。只看最终角度：别的角度上同一个零件的峰会顺着边挪开十几 px，分不清是不是第二处。
  if (pose.score > 0 && !final.window.empty()) {
    double second = 0;
    const std::vector<Peak> peaks = prominentPeaks(final.window, kPeakProminenceMin);
    // 最后一个是整窗最高的峰（突起度无穷大），就是主峰本身
    for (std::size_t i = 0; i + 1 < peaks.size(); ++i)
      second = std::max(second, peaks[i].value);
    pose.peakRatio = std::min(1.0, second / pose.score);
  }

  if (pose.score < spec.minScore)
    return fail("low_score", "匹配分数低于 minScore");
  if (final.atEdge)
    return fail("at_search_edge", "匹配峰贴在搜索窗边上：真实位置可能超出 searchRadius");
  if (angleOff)
    return fail("at_angle_edge", "细扫后的转角超出 angleRange：真实转角可能在扫描范围之外");
  if (pose.peakRatio > spec.maxPeakRatio) {
    return fail("ambiguous", "搜索区里还有一处几乎一样像的匹配（peakRatio 超过 maxPeakRatio），位置不唯一");
  }
  pose.ok = true;
  return pose;
}

CaliperStation measureCaliper(const cv::Mat& gray, P2 c, P2 n, const CaliperSpec& spec) {
  CaliperStation st;
  st.inner = st.nearEdge = st.farEdge = kNaN;
  if (gray.type() != CV_8UC1 || gray.cols < 2 || gray.rows < 2 || !finite(c) || !finite(n)) {
    st.status = "out_of_image";
    return st;
  }
  const double half = std::max(spec.searchHalf, 8.0);
  const int m = static_cast<int>(std::round(4.0 * half)) + 1; // 0.5 px 一步
  auto tOf = [&](int i) { return -half + 0.5 * i; };
  auto iOf = [&](double t) { return static_cast<int>(std::ceil((t + half) * 2.0 - 1e-9)); };

  const P2 across = perp(n);
  const int w = std::max(1, static_cast<int>(std::round(spec.caliperWidth)));
  std::vector<double> raw(m);
  for (int i = 0; i < m; ++i) {
    double acc = 0;
    for (int k = 0; k < w; ++k) {
      const double u = k - (w - 1) / 2.0;
      const P2 p = c + n * tOf(i) + across * u;
      const double v = sample(gray, p.x, p.y);
      if (!std::isfinite(v)) {
        st.status = "out_of_image";
        return st;
      }
      acc += v;
    }
    raw[i] = acc / w;
  }
  // σ = 1 px（两个样本）的高斯
  std::vector<double> v(m);
  {
    constexpr int r = 4;
    double kernel[2 * r + 1];
    for (int k = -r; k <= r; ++k)
      kernel[k + r] = std::exp(-0.5 * (k * k) / 4.0);
    for (int i = 0; i < m; ++i) {
      double acc = 0, wsum = 0;
      for (int k = -r; k <= r; ++k) {
        const int j = i + k;
        if (j < 0 || j >= m)
          continue;
        acc += kernel[k + r] * raw[j];
        wsum += kernel[k + r];
      }
      v[i] = acc / wsum;
    }
  }

  // 内边：±1…3 px 两侧均值之差按极性取正，局部极大、≥ contrastMin 的都是候选
  const double sign = spec.innerDarkToBright ? 1.0 : -1.0;
  auto edgeAt = [&](int i) {
    double lo = 0, hi = 0;
    for (int k = 2; k <= 6; ++k) {
      lo += v[i - k];
      hi += v[i + k];
    }
    return sign * (hi - lo) / 5.0;
  };
  const int a = std::max(iOf(spec.innerFrom), 7);
  const int b = std::min(iOf(spec.innerTo), m - 8);
  int pick = -1;
  double pickE = 0;
  for (int i = a; i <= b; ++i) {
    const double e = edgeAt(i);
    if (e < spec.contrastMin || e < edgeAt(i - 1) || e <= edgeAt(i + 1))
      continue;
    if (pick < 0 || (spec.innerNearest ? i > pick : e > pickE)) {
      pick = i;
      pickE = e;
    }
  }
  if (pick < 0) {
    st.status = "no_inner";
    return st;
  }
  st.inner = tOf(pick) + 0.5 * parabolaPeak(edgeAt(pick - 1), pickE, edgeAt(pick + 1));
  st.innerContrast = pickE;

  // 胶：内边之后的窗口里取最暗处，按本站的半高定两条边
  const int i0 = std::max(iOf(std::max(st.inner + 1.0, spec.beadFrom)), 0);
  const int i1 = std::min(iOf(spec.beadTo), m - 1);
  if (i1 - i0 < 4) {
    st.status = "no_bead";
    return st;
  }
  std::vector<double> d(v.begin() + i0, v.begin() + i1 + 1);
  if (!spec.beadDark) {
    for (double& x : d)
      x = 255.0 - x;
  }
  std::vector<double> sorted = d;
  const std::size_t q80 = (sorted.size() * 4) / 5;
  std::nth_element(sorted.begin(), sorted.begin() + q80, sorted.end());
  const double bg = sorted[q80];
  // 窗口里可能还有别的暗东西（安装孔、阴影）：取离站点（名义胶条中线 t = 0）最近的暗谷，而不是最暗的那个
  const int nd = static_cast<int>(d.size());
  int valley = -1;
  for (int j = 1; j + 1 < nd; ++j) {
    if (d[j] > d[j - 1] || d[j] >= d[j + 1] || bg - d[j] < spec.contrastMin)
      continue;
    if (valley < 0 || std::fabs(tOf(i0 + j)) < std::fabs(tOf(i0 + valley)))
      valley = j;
  }
  if (valley < 0) {
    st.status = "no_bead";
    return st;
  }
  // 把这个暗谷按它自己的半高展开，谷底取展开范围里的最小值（胶底的噪声会造出好几个局部极小）
  int mi = valley;
  {
    const double level0 = 0.5 * (bg + d[valley]);
    int a = valley, b = valley;
    while (a > 0 && d[a - 1] < level0)
      --a;
    while (b + 1 < nd && d[b + 1] < level0)
      ++b;
    mi = static_cast<int>(std::min_element(d.begin() + a, d.begin() + b + 1) - d.begin());
  }
  st.beadContrast = bg - d[mi];
  const double level = 0.5 * (bg + d[mi]);
  // 没有跨过半高的边不能用窗口边界代替；否则会把截断的胶宽当成合格量测。
  for (int j = mi; j >= 0; --j) {
    if (d[j] >= level) {
      const double frac = (d[j] - level) / std::max(d[j] - d[j + 1], 1e-9);
      st.nearEdge = tOf(i0 + j) + 0.5 * frac;
      break;
    }
  }
  for (int j = mi; j < static_cast<int>(d.size()); ++j) {
    if (d[j] >= level) {
      const double frac = (level - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-9);
      st.farEdge = tOf(i0 + j - 1) + 0.5 * frac;
      break;
    }
  }
  if (!std::isfinite(st.nearEdge) || !std::isfinite(st.farEdge)) {
    st.status = "incomplete_bead";
    st.nearEdge = st.farEdge = kNaN;
  } else if (st.farEdge - st.nearEdge < spec.widthMin) {
    st.status = "no_bead";
    st.nearEdge = st.farEdge = kNaN;
  }
  return st;
}

} // namespace lyflow::packs::glue
