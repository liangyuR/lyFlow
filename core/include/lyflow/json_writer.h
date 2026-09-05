#pragma once
// 最小 JSON 写出器。流式写出（键顺序由调用方定）+ 最短往返浮点，
// 两条要求来自 docs/graph-doc.md「可稳定序列化」。
#include <cstdint>
#include <string>
#include <vector>

namespace lyflow {

class JsonWriter {
 public:
  JsonWriter() = default;

  // 容器
  void beginObject();
  void endObject();
  void beginArray();
  void endArray();

  // 对象成员：先写键，再写值
  void key(const std::string& k);

  // 值
  void valueNull();
  void value(bool v);
  void value(std::int64_t v);
  void value(int v) { value(static_cast<std::int64_t>(v)); }
  void value(double v);
  void value(const char* v);
  void value(const std::string& v);
  /// 原样嵌入一段已经序列化好的 JSON。调用方负责它是合法的。
  void raw(const std::string& json);

  // 便捷：写一个 "key": value 对
  void field(const std::string& k, bool v);
  void field(const std::string& k, std::int64_t v);
  void field(const std::string& k, int v) { field(k, static_cast<std::int64_t>(v)); }
  void field(const std::string& k, double v);
  void field(const std::string& k, const std::string& v);

  // 便捷：字符串非空时才写。manifest 里大量可选字段用这个，
  // 避免输出一堆 "doc": "" 噪声。
  void fieldIfSet(const std::string& k, const std::string& v);
  void fieldIfSet(const std::string& k, const std::vector<std::string>& v);

  void setIndent(int spaces) { indentWidth_ = spaces; }

  std::string str() const { return out_; }

 private:
  void prepareValue();
  void newlineIndent();

  std::string out_;
  std::vector<bool> stack_;      // true = array, false = object
  bool needComma_ = false;
  bool afterKey_ = false;
  int indentWidth_ = 2;
};

// 把字符串转义成 JSON 字符串字面量（含首尾引号）。
// 输入必须是合法 UTF-8；非 ASCII 字节原样透传，不做 \u 转义。
std::string jsonEscape(const std::string& s);

// 最短往返的浮点表示。保证 round-trip，且整数值带 .0 以免在
// JS 侧被误当成整数（前端要靠 param.type 而不是值的形态来判断类型）。
std::string jsonNumber(double v);

}  // namespace lyflow
