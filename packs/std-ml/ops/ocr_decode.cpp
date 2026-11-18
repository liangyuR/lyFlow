#include <algorithm>
#include <fstream>

#include "lyflow_ml/ocr_contract.h"
#include "ops.h"

namespace lyflow::ops {
namespace {

using namespace ocr;

void orient(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  auto metadata = layout(in);
  const auto* input = in.get("recInput").asTensor();
  finite(input, "recInput", ctx);
  const auto* classification = in.get("classification").asTensor();
  finite(classification, "classification", ctx);
  require(input->shape == std::vector<std::int64_t>{static_cast<std::int64_t>(metadata.count), 3,
                                                    48, metadata.width},
          "recInput与layout不匹配", "recInput");
  require((metadata.count == 0 && classification->data.empty()) ||
              classification->shape ==
                  std::vector<std::int64_t>{static_cast<std::int64_t>(metadata.count), 2},
          "方向输出须为[N,2]且类别顺序为0度/180度", "classification");
  const double threshold = number(p, "minConfidence", 0, 1);
  Tensor output = *input;
  for (float value : input->data)
    require(value >= -1 && value <= 1, "recInput须为-1..1归一化像素", "recInput");
  const auto plane = static_cast<std::size_t>(48) * metadata.width;
  for (std::size_t i = 0; i < metadata.count; ++i) {
    check(ctx);
    const double a = classification->data[i * 2], b = classification->data[i * 2 + 1];
    require(a >= 0 && a <= 1 && b >= 0 && b <= 1 && std::abs(a + b - 1) <= .001,
            "方向分类必须为两类归一化概率", "classification");
    auto& item = metadata.data["items"][i];
    require(item["orientation"] == 0 && item["orientationConfidence"] == 0,
            "layout已经应用方向分类，不可重复翻转", "layout");
    const bool reverse = b > a && b >= threshold;
    item["orientation"] = reverse ? 180 : 0;
    item["classifierLabel"] = b > a ? 180 : 0;
    item["orientationConfidence"] = std::max(a, b);
    if (reverse) {
      const int validWidth = item["validWidth"];
      for (int c = 0; c < 3; ++c)
        for (int y = 0; y < 48; ++y)
          for (int x = 0; x < validWidth; ++x)
            output.data[(i * 3 + c) * plane + y * metadata.width + x] =
                input->data[(i * 3 + c) * plane + (47 - y) * metadata.width + validWidth - 1 - x];
    }
  }
  Record record{kLayout, std::move(metadata.data)};
  bounded(record);
  out.set("input", Data::tensor(std::move(output)));
  out.set("layout", Data::record(std::move(record)));
}

std::vector<std::string> dictionary(const ParamView& p) {
  const auto path = p.path("dictionaryPath");
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec || bytes > 4 * 1024 * 1024)
    throw Failure{op_params::badParam("字典缺失或超过4MiB", "dictionaryPath")};
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw Failure{op_params::badParam("无法读取UTF-8字典", "dictionaryPath")};
  std::vector<std::string> words;
  std::set<std::string> unique;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (words.empty() && line.compare(0, 3, "\xef\xbb\xbf") == 0)
      line.erase(0, 3);
    std::string why;
    if (line.empty() || line.size() > 256 || !values::boundedJson(line, why) ||
        !unique.insert(line).second)
      throw Failure{
          op_params::badParam("字典每行须为唯一、非空、至多256字节的UTF-8符号", "dictionaryPath")};
    words.push_back(line);
    limit(words.size() <= 65534, "字典符号超过65534项");
  }
  if (words.empty())
    throw Failure{op_params::badParam("字典不能为空", "dictionaryPath")};
  if (p.flag("appendSpace")) {
    if (unique.count(" "))
      throw Failure{op_params::badParam("字典已有空格，appendSpace须关闭", "appendSpace")};
    words.emplace_back(" ");
  }
  words.insert(words.begin(), "");
  return words;
}

void ctc(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  const auto metadata = layout(in);
  const auto words = dictionary(p);
  const auto* prediction = in.get("prediction").asTensor();
  finite(prediction, "prediction", ctx);
  const auto axis = p.choice("axisOrder"), mode = p.choice("outputMode"),
             characterSet = p.choice("characterSet");
  if ((axis != "NTC" && axis != "TNC") || (mode != "probabilities" && mode != "logits") ||
      (characterSet != "all" && characterSet != "digits" && characterSet != "lowercase" &&
       characterSet != "uppercase" && characterSet != "letters"))
    throw Failure{op_params::badParam("CTC布局、概率模式或字符集非法", "axisOrder")};
  const double minCharacter = number(p, "minCharacterScore", 0, 1),
               minText = number(p, "minTextScore", 0, 1);
  const auto separator = p.text("separator");
  if (separator.size() > 16)
    throw Failure{op_params::badParam("分隔符超过16字节", "separator")};
  const bool empty = metadata.count == 0;
  require(empty ? prediction->data.empty() : prediction->shape.size() == 3,
          "CTC输出必须为明确的NTC/TNC三维张量", "prediction");
  int steps = 0;
  if (!empty) {
    const auto& shape = prediction->shape;
    require(shape[axis == "NTC" ? 0 : 1] == static_cast<std::int64_t>(metadata.count) &&
                shape[2] == static_cast<std::int64_t>(words.size()) &&
                shape[axis == "NTC" ? 1 : 0] > 0 && shape[axis == "NTC" ? 1 : 0] <= 8192,
            "CTC批次、字典类别数或时间步不匹配；blank固定为0", "prediction");
    steps = static_cast<int>(shape[axis == "NTC" ? 1 : 0]);
  }
  Json items = Json::array();
  std::string combined;
  const auto classes = words.size();
  for (std::size_t n = 0; n < metadata.count; ++n) {
    check(ctx);
    std::string raw, filtered;
    double confidence = 0;
    std::size_t emitted = 0, previous = classes;
    for (int t = 0; t < steps; ++t) {
      if (!(t % 32))
        check(ctx);
      const auto offset =
          (axis == "NTC" ? n * steps + t : static_cast<std::size_t>(t) * metadata.count + n) *
          classes;
      const auto begin = prediction->data.begin() + offset;
      const auto winner =
          static_cast<std::size_t>(std::max_element(begin, begin + classes) - begin);
      double probability = prediction->data[offset + winner], sum = 0;
      if (mode == "logits") {
        for (std::size_t c = 0; c < classes; ++c)
          sum += std::exp(prediction->data[offset + c] - probability);
        probability = 1 / sum;
      } else {
        for (std::size_t c = 0; c < classes; ++c) {
          const float value = prediction->data[offset + c];
          require(value >= 0 && value <= 1, "CTC概率须为0..1", "prediction");
          sum += value;
        }
        require(std::abs(sum - 1) <= .001, "CTC概率行和须为1；logits须显式选择", "prediction");
      }
      if (winner && winner != previous) {
        const auto& token = words[winner];
        raw += token;
        confidence += probability;
        ++emitted;
        if (probability >= minCharacter) {
          for (std::size_t at = 0; at < token.size();) {
            const auto first = static_cast<unsigned char>(token[at]);
            const std::size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            const bool digit = length == 1 && first >= '0' && first <= '9';
            const bool lower = length == 1 && first >= 'a' && first <= 'z';
            const bool upper = length == 1 && first >= 'A' && first <= 'Z';
            const bool allowed = characterSet == "all" || digit ||
                                 (characterSet == "lowercase" && lower) ||
                                 (characterSet == "uppercase" && upper) ||
                                 (characterSet == "letters" && (lower || upper));
            if (allowed)
              filtered.append(token, at, length);
            at += length;
          }
        }
        limit(raw.size() <= values::kMaxStringBytes && filtered.size() <= values::kMaxStringBytes,
              "OCR文字超过64KiB；不会截断内容");
      }
      previous = winner;
    }
    confidence = emitted ? confidence / emitted : 0;
    const bool accepted = !filtered.empty() && confidence >= minText;
    auto item = metadata.data["items"][n];
    item["rawText"] = raw;
    item["text"] = filtered;
    item["confidence"] = confidence;
    item["accepted"] = accepted;
    items.push_back(std::move(item));
    if (accepted) {
      if (!combined.empty())
        combined += separator;
      combined += filtered;
    }
    limit(combined.size() <= values::kMaxStringBytes, "合并OCR文字超过64KiB");
  }
  Record results{kResults,
                 {{"frame", metadata.data["frame"]},
                  {"unit", "px"},
                  {"geometryHash", metadata.data["geometryHash"]},
                  {"items", items}}};
  auto value = values::string(combined);
  bounded(results);
  bounded(value);
  out.set("text", Data::record(std::move(value)));
  out.set("results", Data::record(std::move(results)));
}

std::string dictionaryKey(const ParamView& p) {
  std::error_code ec;
  const auto path = p.path("dictionaryPath");
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec)
    return {};
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec)
    return {};
  return path.generic_u8string() + ":" + std::to_string(bytes) + ":" +
         std::to_string(time.time_since_epoch().count());
}
} // namespace

void registerOcrDecode(Registry& registry) {
  auto direction = descriptor("image.ocr_orient", "文字方向校正", orient);
  direction.inputs = {Port{"recInput", "Tensor", "识别像素", "[N,3,48,W]", true},
                      Port{"classification", "Tensor", "方向概率", "[N,2] 0度/180度", true},
                      op_params::recordPort("layout", kLayout)};
  direction.outputs = {Port{"input", "Tensor", "识别输入", "[N,3,48,W]", true},
                       op_params::recordPort("layout", kLayout)};
  direction.params = {real("minConfidence", "方向置信阈值", .9, 0, 1)};
  direction.doc =
      "仅按可信180度类别翻转有效文字像素，右侧padding位置保持；不重复应用方向，空批次接受ONNX serial的canonical empty输出。";
  registry.addOperator(std::move(direction));
  auto decode = descriptor("image.ocr_ctc_decode", "文字CTC解码", ctc);
  decode.inputs = {Port{"prediction", "Tensor", "字符输出", "显式NTC/TNC", true},
                   op_params::recordPort("layout", kLayout)};
  decode.outputs = {op_params::recordPort("text", values::kValueType),
                    op_params::recordPort("results", kResults)};
  Param dict = op_params::text("dictionaryPath", "UTF-8字典", "");
  dict.type = ParamType::Path;
  dict.mode = "open";
  dict.filters = {FileFilter{"字典", {"txt"}}};
  decode.params = {
      dict,
      op_params::boolParam("appendSpace", "末尾追加空格类", true, "blank固定0，其后按字典原行序"),
      op_params::enumParam("axisOrder", "输出布局", "NTC",
                           {{"NTC", "NTC", "批次/时间/类别"}, {"TNC", "TNC", "时间/批次/类别"}},
                           ""),
      op_params::enumParam(
          "outputMode", "输出数值", "probabilities",
          {{"probabilities", "概率", "每行概率和为1"}, {"logits", "Logits", "显式稳定softmax"}},
          ""),
      real("minCharacterScore", "字符最小置信", 0, 0, 1),
      real("minTextScore", "文字最小置信", .5, 0, 1),
      op_params::enumParam("characterSet", "字符过滤", "all",
                           {{"all", "全部Unicode", "不按UTF-8字节拆分"},
                            {"digits", "数字", "ASCII数字"},
                            {"lowercase", "数字与小写", ""},
                            {"uppercase", "数字与大写", ""},
                            {"letters", "数字与字母", ""}},
                           ""),
      op_params::text("separator", "行分隔符", "\n")};
  decode.externalKey = dictionaryKey;
  decode.doc =
      "严格UTF-8字典和类别数配套，blank=0，先折叠CTC重复再删除blank；保留逐行rawText/置信/过滤结果。低分行不进入合并文本，但结果条目保留；无目标返回成功空文本。";
  registry.addOperator(std::move(decode));
}
} // namespace lyflow::ops
