#include "lyflow/json_writer.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lyflow {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          // 其余控制字符走 \u00XX。非 ASCII (>= 0x80) 是 UTF-8 续字节，
          // 原样透传 —— JSON 本身就是 UTF-8，转义只会让 doc 里的中文变得不可读。
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string jsonNumber(double v) {
  // JSON 没有 NaN/Infinity。写裸的 nan/inf 会让整份文档解析失败，
  // 写 null 至少合法，前端能看出「这里有个值但它不是数」。
  if (!std::isfinite(v)) return "null";

  // to_chars 的 general 格式给出最短往返表示，没有 printf("%g") 的精度损失，
  // 也没有 printf("%.17g") 的 0.30000000000000004 噪声。
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  if (res.ec != std::errc()) {
    return "0";
  }
  std::string s(buf, res.ptr);

  // 整数值补 .0。GraphDoc 的 diff 稳定性依赖于同一个值总是写成同样的字节，
  // 而 leafSize 从 0.01 调到 1 时不应该让类型形态发生变化。
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) {
    s += ".0";
  }
  return s;
}

void JsonWriter::newlineIndent() {
  if (indentWidth_ <= 0) return;
  out_.push_back('\n');
  out_.append(static_cast<std::size_t>(indentWidth_) * stack_.size(), ' ');
}

void JsonWriter::prepareValue() {
  if (afterKey_) {
    afterKey_ = false;
    return;  // 键后面的值紧跟冒号，不换行
  }
  if (needComma_) out_.push_back(',');
  if (!stack_.empty()) newlineIndent();
  needComma_ = true;
}

void JsonWriter::beginObject() {
  prepareValue();
  out_.push_back('{');
  stack_.push_back(false);
  needComma_ = false;
}

void JsonWriter::endObject() {
  bool empty = !needComma_;
  stack_.pop_back();
  if (!empty) newlineIndent();
  out_.push_back('}');
  needComma_ = true;
}

void JsonWriter::beginArray() {
  prepareValue();
  out_.push_back('[');
  stack_.push_back(true);
  needComma_ = false;
}

void JsonWriter::endArray() {
  bool empty = !needComma_;
  stack_.pop_back();
  if (!empty) newlineIndent();
  out_.push_back(']');
  needComma_ = true;
}

void JsonWriter::key(const std::string& k) {
  if (needComma_) out_.push_back(',');
  newlineIndent();
  out_ += jsonEscape(k);
  out_.push_back(':');
  if (indentWidth_ > 0) out_.push_back(' ');
  needComma_ = true;
  afterKey_ = true;
}

void JsonWriter::valueNull()                  { prepareValue(); out_ += "null"; }
void JsonWriter::value(bool v)                { prepareValue(); out_ += v ? "true" : "false"; }
void JsonWriter::value(std::int64_t v)        { prepareValue(); out_ += std::to_string(v); }
void JsonWriter::value(double v)              { prepareValue(); out_ += jsonNumber(v); }
void JsonWriter::value(const char* v)         { prepareValue(); out_ += jsonEscape(v ? v : ""); }
void JsonWriter::value(const std::string& v)  { prepareValue(); out_ += jsonEscape(v); }

void JsonWriter::field(const std::string& k, bool v)               { key(k); value(v); }
void JsonWriter::field(const std::string& k, std::int64_t v)       { key(k); value(v); }
void JsonWriter::field(const std::string& k, double v)             { key(k); value(v); }
void JsonWriter::field(const std::string& k, const std::string& v) { key(k); value(v); }

void JsonWriter::fieldIfSet(const std::string& k, const std::string& v) {
  if (!v.empty()) field(k, v);
}

void JsonWriter::fieldIfSet(const std::string& k, const std::vector<std::string>& v) {
  if (v.empty()) return;
  key(k);
  beginArray();
  for (const auto& s : v) value(s);
  endArray();
}

}  // namespace lyflow
