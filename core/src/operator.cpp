#include "lyflow/operator.h"

#include <stdexcept>

namespace lyflow {
namespace {

[[noreturn]] void bug(std::string_view name, const char* what) {
  throw std::logic_error(std::string("参数 '") + std::string(name) + "' " + what +
                         "（执行器应当已经校验并合并过默认值，走到这里说明是 core 的 bug）");
}

}  // namespace

const char* toString(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

// ------------------------------------------------------------------ ParamView

const Value& ParamView::lookup(std::string_view name) const {
  auto it = values_->find(std::string(name));
  if (it == values_->end()) bug(name, "不存在");
  return it->second;
}

bool ParamView::has(std::string_view name) const {
  return values_->find(std::string(name)) != values_->end();
}

bool ParamView::flag(std::string_view name) const {
  const Value& v = lookup(name);
  if (v.kind() != Value::Kind::Bool) bug(name, "不是 bool");
  return v.boolValue();
}

std::int64_t ParamView::integer(std::string_view name) const {
  const Value& v = lookup(name);
  if (v.kind() == Value::Kind::Int) return v.intValue();
  if (v.kind() == Value::Kind::Float) return static_cast<std::int64_t>(v.floatValue());
  bug(name, "不是整数");
}

double ParamView::number(std::string_view name) const {
  const Value& v = lookup(name);
  if (v.kind() == Value::Kind::Float) return v.floatValue();
  if (v.kind() == Value::Kind::Int) return static_cast<double>(v.intValue());
  bug(name, "不是数字");
}

const std::string& ParamView::text(std::string_view name) const {
  const Value& v = lookup(name);
  if (v.kind() != Value::Kind::String) bug(name, "不是字符串");
  return v.stringValue();
}

const std::string& ParamView::choice(std::string_view name) const { return text(name); }

namespace {

template <std::size_t N>
std::array<float, N> vecOf(const Value& v, std::string_view name) {
  if (v.kind() != Value::Kind::FloatVec || v.vecValue().size() != N) {
    bug(name, "不是期望长度的浮点向量");
  }
  std::array<float, N> out{};
  for (std::size_t i = 0; i < N; ++i) out[i] = static_cast<float>(v.vecValue()[i]);
  return out;
}

}  // namespace

std::array<float, 2> ParamView::vec2(std::string_view name) const { return vecOf<2>(lookup(name), name); }
std::array<float, 3> ParamView::vec3(std::string_view name) const { return vecOf<3>(lookup(name), name); }
std::array<float, 4> ParamView::vec4(std::string_view name) const { return vecOf<4>(lookup(name), name); }

std::filesystem::path ParamView::path(std::string_view name) const {
  const std::string& raw = text(name);
  if (raw.empty()) return {};
  // u8path 而不是 path(std::string)：后者按「本地窄编码」解释，在 936 代码页下
  // 会把 UTF-8 的中文路径当 GBK 读，得到一个根本不存在的路径。
  std::filesystem::path p = std::filesystem::u8path(raw);
  if (p.is_absolute() || baseDir_->empty()) return p;
  return (*baseDir_) / p;
}

// --------------------------------------------------------------------- Inputs

bool Inputs::has(std::string_view name) const {
  auto it = values_->find(std::string(name));
  return it != values_->end() && !it->second.empty();
}

const Data& Inputs::get(std::string_view name) const {
  static const Data kEmpty;
  auto it = values_->find(std::string(name));
  return it == values_->end() ? kEmpty : it->second;
}

// -------------------------------------------------------------------- Outputs

void Outputs::set(std::string_view name, Data value) {
  (*values_)[std::string(name)] = std::move(value);
}

}  // namespace lyflow
