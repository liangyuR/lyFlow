#include <doctest/doctest.h>

#include <BarcodeData.h>
#include <BitMatrix.h>
#include <DecoderResult.h>
#include <qrcode/QRDecoder.h>
#include <qrcode/QRErrorCorrectionLevel.h>
#include <qrcode/QRVersion.h>
#include <opencv2/objdetect.hpp>
#include <random>

namespace {
ZXing::BitMatrix emptyQrModules() {
  cv::QRCodeEncoder::Params parameters;
  parameters.version = 1;
  parameters.mode = cv::QRCodeEncoder::MODE_BYTE;
  parameters.correction_level = cv::QRCodeEncoder::CORRECT_LEVEL_L;
  cv::Mat encoded;
  cv::QRCodeEncoder::create(parameters)->encode("", encoded);
  REQUIRE(encoded.rows == 25);
  REQUIRE(encoded.cols == 25);
  ZXing::BitMatrix result(21, 21);
  for (int y = 0; y < 21; ++y)
    for (int x = 0; x < 21; ++x)
      result.set(x, y, encoded.at<std::uint8_t>(y + 2, x + 2) == 0);
  return result;
}
} // namespace

TEST_CASE("ZXing empty QR validity requires complete Model2 correction and payload parsing") {
  auto modules = emptyQrModules();
  auto decoded = ZXing::QRCode::Decode(modules);
  REQUIRE_FALSE(static_cast<bool>(decoded.error()));
  CHECK(decoded.content().bytes.empty());
  CHECK(decoded.versionNumber() == 1);
  CHECK(decoded.isValid());
  CHECK(decoded.hasValidEmptyQRCode());
  const ZXing::QuadrilateralI corners({0, 0}, {20, 0}, {20, 20}, {0, 20});
  ZXing::DetectorResult geometry(std::move(modules), ZXing::QuadrilateralI(corners));
  const auto barcode =
      ZXing::MatrixBarcode(std::move(decoded), std::move(geometry), ZXing::BarcodeFormat::QRCode);
  CHECK(barcode.content.bytes.empty());
  CHECK(barcode.validEmptyQRCode);
  CHECK(barcode.isValid());

  ZXing::DecoderResult defaultResult;
  CHECK_FALSE(defaultResult.isValid());
  CHECK_FALSE(defaultResult.hasValidEmptyQRCode());
  ZXing::BarcodeData defaultBarcode;
  CHECK_FALSE(defaultBarcode.isValid());
  defaultBarcode.format = ZXing::BarcodeFormat::QRCode;
  CHECK_FALSE(defaultBarcode.isValid());
  defaultBarcode.validEmptyQRCode = true;
  defaultBarcode.format = ZXing::BarcodeFormat::DataMatrix;
  CHECK_FALSE(defaultBarcode.isValid());
  defaultBarcode.format = ZXing::BarcodeFormat::QRCode;
  defaultBarcode.error = ZXing::Error(ZXing::Error::Checksum);
  CHECK_FALSE(defaultBarcode.isValid());
}

TEST_CASE("ZXing empty QR patch preserves damaged random and default failure diagnostics") {
  auto damaged = emptyQrModules();
  for (int y = 9; y < 21; ++y)
    for (int x = 9; x < 21; ++x)
      damaged.set(x, y, (x * 5 + y * 7) % 3 == 0);
  const auto decoded = ZXing::QRCode::Decode(damaged);
  CHECK(static_cast<bool>(decoded.error()));
  CHECK_FALSE(decoded.isValid());
  CHECK_FALSE(decoded.hasValidEmptyQRCode());
  CHECK(decoded.isValid(true));
  for (unsigned seed = 0; seed < 16; ++seed) {
    CAPTURE(seed);
    std::mt19937 random(seed);
    ZXing::BitMatrix noise(21, 21);
    for (int y = 0; y < 21; ++y)
      for (int x = 0; x < 21; ++x)
        noise.set(x, y, (random() & 1U) != 0);
    const auto result = ZXing::QRCode::Decode(noise);
    CHECK_FALSE(result.isValid());
    CHECK_FALSE(result.hasValidEmptyQRCode());
    CHECK(static_cast<bool>(result.error()));
  }
  const auto wrongSize = ZXing::QRCode::Decode(ZXing::BitMatrix(20, 20));
  CHECK(static_cast<bool>(wrongSize.error()));
  CHECK_FALSE(wrongSize.isValid());
  CHECK_FALSE(wrongSize.hasValidEmptyQRCode());
}
