#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <nlohmann/json.hpp>

#include "../algo/p2_matching.h"
#include "data/gicp_stationary_cases.h"

TEST_CASE("GICP captured numerical plateaus meet the original undamped step thresholds") {
  using namespace lyflow::algo::p2;
  const auto cases = nlohmann::json::parse(lyflow::test::kGicpStationaryCases);
  REQUIRE(cases.size() == 3);
  for (const auto& value : cases) {
    const auto hessian = value.at("HColumnMajor").get<std::array<double, 36>>();
    const auto gradient = value.at("b").get<std::array<double, 6>>();
    GicpBackendOptions options;
    options.voxelSize = value.at("voxelSize");
    options.translationTolerance = value.at("translationTolerance");
    options.rotationTolerance = value.at("rotationTolerance");
    options.maxIterations = value.at("maxIterations");
    const auto h = Eigen::Map<const Eigen::Matrix<double, 6, 6>>(hessian.data());
    const auto b = Eigen::Map<const Eigen::Matrix<double, 6, 1>>(gradient.data());
    CHECK(stationaryGicpStep(h, b, value.at("error"), value.at("inliers"), value.at("iteration"),
                             options));
    CHECK_FALSE(stationaryGicpStep(h, b * 1e9, value.at("error"), value.at("inliers"),
                                   value.at("iteration"), options));
    options.maxIterations = value.at("iteration").get<int>() + 1;
    CHECK_FALSE(stationaryGicpStep(h, b, value.at("error"), value.at("inliers"),
                                   value.at("iteration"), options));
  }
}

TEST_CASE("GICP stationary qualification uses an undamped full rank step and original tolerances") {
  using namespace lyflow::algo::p2;
  using Hessian = Eigen::Matrix<double, 6, 6>;
  using Gradient = Eigen::Matrix<double, 6, 1>;
  const Hessian identity = Hessian::Identity();
  GicpBackendOptions options;
  options.voxelSize = .001;
  options.translationTolerance = 1e-7;
  options.rotationTolerance = 1e-7;
  options.maxIterations = 50;
  Gradient gradient = Gradient::Zero();
  gradient[0] = 5e-8;
  gradient[3] = 5e-7;
  const auto accepted = [&](const Hessian& hessian, const Gradient& value, double error = .03,
                            std::size_t count = 100, std::size_t iteration = 2) {
    return stationaryGicpStep(hessian, value, error, count, iteration, options);
  };
  CHECK(accepted(identity, gradient));
  CHECK_FALSE(accepted(identity, gradient, .03, 3));
  CHECK_FALSE(accepted(identity, gradient, .03, 100, 49));
  CHECK_FALSE(accepted(identity, gradient, .03, 100, 50));
  CHECK(accepted(identity, gradient, .03, 100, 48));
  CHECK_FALSE(accepted(identity, gradient, -1));
  CHECK_FALSE(accepted(identity, gradient, std::numeric_limits<double>::infinity()));
  auto changed = gradient;
  changed[0] = 2e-7;
  CHECK_FALSE(accepted(identity, changed));
  changed = gradient;
  changed[3] = 2e-6;
  CHECK_FALSE(accepted(identity, changed));
  changed[3] = 1;
  CHECK_FALSE(accepted(identity, changed));
  changed = gradient;
  changed[1] = std::numeric_limits<double>::quiet_NaN();
  CHECK_FALSE(accepted(identity, changed));
  Hessian hessian = identity;
  hessian(0, 0) = -1;
  CHECK_FALSE(accepted(hessian, gradient));
  hessian(0, 0) = 0;
  CHECK_FALSE(accepted(hessian, gradient));
  hessian(0, 0) = 1e-13;
  CHECK_FALSE(accepted(hessian, gradient));
  hessian = identity;
  hessian(0, 1) = .1;
  CHECK_FALSE(accepted(hessian, gradient));
  hessian(0, 1) = std::numeric_limits<double>::infinity();
  CHECK_FALSE(accepted(hessian, gradient));
  CHECK_FALSE(accepted(Hessian::Zero(), Gradient::Zero()));
  options.voxelSize = .01;
  CHECK_FALSE(accepted(identity, gradient));
  options.voxelSize = .001;
  options.maxIterations = 1;
  CHECK_FALSE(accepted(identity, gradient, .03, 100, 0));
  options.maxIterations = 50;
  options.rotationTolerance = 0;
  CHECK_FALSE(accepted(identity, gradient));
}
