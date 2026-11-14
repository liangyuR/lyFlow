#include "backend.h"

#include <Barcode.h>
#include <BarcodeFormat.h>
#include <ContentType.h>
#include <ImageView.h>
#include <ReadBarcode.h>
#include <ReaderOptions.h>
#include <stdexcept>

namespace lyflow::barcode {
namespace {
std::string formatName(ZXing::BarcodeFormat format) {
  switch (format) {
#define ZX_NAME(NAME, ...)                                                                         \
  case ZXing::BarcodeFormat::NAME:                                                                 \
    return #NAME;
    ZX_BCF_LIST(ZX_NAME)
#undef ZX_NAME
  default:
    return "Unknown";
  }
}
} // namespace
std::vector<Decoded> read(const std::uint8_t* gray, int width, int height, int rowStride,
                          const Options& options) {
  if (options.limit < 1 || options.limit > 255)
    throw std::invalid_argument("ZXing result budget must be 1..255");
  auto formats = options.formats;
  if (formats == "Any")
    formats = "AllReadable";
  else if (formats == "Linear")
    formats = "AllLinear";
  else if (formats == "Matrix")
    formats = "AllMatrix";
  ZXing::ReaderOptions settings;
  settings.formats(ZXing::BarcodeFormats(formats))
      .tryHarder(options.tryHarder)
      .tryRotate(options.tryRotate)
      .tryInvert(options.tryInvert)
      .tryDownscale(options.tryDownscale)
      .returnErrors(true)
      .textMode(ZXing::TextMode::Plain)
      .maxNumberOfSymbols(static_cast<std::uint8_t>(options.limit));
  const auto found = ZXing::ReadBarcodes(
      ZXing::ImageView(gray, width, height, ZXing::ImageFormat::Lum, rowStride), settings);
  std::vector<Decoded> result;
  result.reserve(found.size());
  for (const auto& code : found) {
    Decoded item;
    item.valid = code.isValid();
    item.format = formatName(code.format());
    item.text = code.text();
    item.bytes = code.bytes();
    item.contentType = ZXing::ToString(code.contentType());
    item.error = ZXing::ToString(code.error());
    item.inverted = code.isInverted();
    item.mirrored = code.isMirrored();
    item.hasECI = code.hasECI();
    item.orientation = code.orientation();
    item.lineCount = code.lineCount();
    const auto& corners = code.position();
    for (int i = 0; i < 4; ++i)
      item.corners[i] = {static_cast<double>(corners[i].x), static_cast<double>(corners[i].y)};
    result.push_back(std::move(item));
  }
  return result;
}
} // namespace lyflow::barcode
