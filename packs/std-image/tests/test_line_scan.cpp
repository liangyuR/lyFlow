#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "lyflow_cv/line_scan.h"

namespace line_scan = lyflow::std_image::line_scan;
using line_scan::P2;

namespace {

/// 一行一行都一样的响应剖面：t 处（相对 x = 200）的值由 profile 给，法向 n = +x。
line_scan::Field profileField(const std::function<double(double)>& profile) {
  cv::Mat m(5, 401, CV_32F);
  for (int x = 0; x < m.cols; ++x) {
    for (int y = 0; y < m.rows; ++y)
      m.at<float>(y, x) = static_cast<float>(profile(x - 200.0));
  }
  return line_scan::Field::whole(m);
}

} // namespace

TEST_CASE("图像法向采样：响应分段、边候选与截断") {
  // 胶 [−20, 20] 峰 60；浅谷 (20, 35) 响应 12（> segmentMin 8、< 半高 30）；灰暗区 [35, 85] 响应 40
  const line_scan::Field R = profileField([](double t) {
    if (t >= -20 && t <= 20)
      return 60.0;
    if (t > 20 && t < 35)
      return 12.0;
    if (t >= 35 && t <= 85)
      return 40.0;
    return 0.0;
  });
  const P2 c(200, 2), n(1, 0);
  line_scan::RunSplitSpec spec;
  spec.mergeWidthMax = 200;
  SUBCASE("connected：胶与灰暗区连成一段") {
    const auto runs = line_scan::caliperRuns(R, c, n, 100, spec);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].width() > 100);
  }
  SUBCASE("half_max：拆成两段，胶那一段（像素 −20…20，41 px）居中") {
    spec.halfMax = true;
    std::vector<line_scan::Run> raw;
    double valid = 0;
    const auto samples = line_scan::sampleCaliper(R, c, n, 100);
    const auto runs = line_scan::caliperRuns(samples, 100, spec, &raw, &valid);
    REQUIRE(runs.size() == 2);
    CHECK(std::fabs(runs[0].width() - 41) <= 0.5);
    CHECK(std::fabs(runs[0].mid()) <= 0.5);
    CHECK(runs[0].peak == doctest::Approx(60));
    CHECK(runs[0].loComplete);
    CHECK(runs[0].hiComplete);
    CHECK(runs[1].peak == doctest::Approx(40));
    CHECK(raw.size() == 2);
    CHECK(valid == 1.0);
  }
  SUBCASE("half_max + 合并缝 20：峰值比 0.4 并回去，0.8 不并") {
    spec.halfMax = true;
    spec.mergeGap = 20;
    spec.mergePeakRatio = 0.4;
    auto runs = line_scan::caliperRuns(R, c, n, 100, spec);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].parts == 2);
    spec.mergePeakRatio = 0.8;
    runs = line_scan::caliperRuns(R, c, n, 100, spec);
    CHECK(runs.size() == 2);
  }
  SUBCASE("connected 但分段门限 13 高过谷底：两段") {
    spec.segmentMin = 13;
    CHECK(line_scan::caliperRuns(R, c, n, 100, spec).size() == 2);
  }
  SUBCASE("卡尺端点截断：灰暗区伸出 searchHalf 70，hi 一侧不完整") {
    spec.halfMax = true;
    const auto runs = line_scan::caliperRuns(R, c, n, 70, spec);
    REQUIRE(runs.size() == 2);
    CHECK(runs[1].loComplete);
    CHECK_FALSE(runs[1].hiComplete);
  }
  SUBCASE("沿法向给出全部边候选，采样出图即截断") {
    line_scan::EdgeScanSpec edges;
    edges.contrastMin = 25.0;
    edges.farRun = 8.0;
    int samples = 0;
    const auto hits = line_scan::findPartEdges(R, c, n, 0.0, 300.0, edges, false, &samples);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].t == doctest::Approx(20.5).epsilon(0.03));
    CHECK(hits[1].t == doctest::Approx(85.5).epsilon(0.03));
    CHECK(samples == 200);
  }
}

TEST_CASE("图像扇形扫描：没有有效方向时默认不出峰，includeInvalid 时按角度顺序补") {
  const line_scan::Field none = line_scan::Field::whole(
      cv::Mat(200, 200, CV_32F, cv::Scalar(std::numeric_limits<float>::quiet_NaN())));
  line_scan::FanSpec spec;
  spec.anchor = P2(100, 100);
  spec.r0 = 10;
  spec.r1 = 50;
  CHECK(line_scan::fanScan(none, spec).peaks.empty());
  spec.includeInvalid = true;
  const line_scan::FanResult fan = line_scan::fanScan(none, spec);
  REQUIRE(fan.peaks.size() == 3);
  CHECK(fan.peaks[0].angle == -180.0);
  CHECK(fan.peaks[1].angle == -164.0);
  CHECK(fan.peaks[2].angle == -148.0);
  CHECK(std::isnan(fan.peaks[0].score));
}
