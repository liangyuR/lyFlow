#include "lyflow/manifest.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lyflow {

Value Value::boolean(bool v) {
  Value x; x.kind_ = Kind::Bool; x.bool_ = v; return x;
}
Value Value::integer(std::int64_t v) {
  Value x; x.kind_ = Kind::Int; x.int_ = v; return x;
}
Value Value::number(double v) {
  Value x; x.kind_ = Kind::Float; x.float_ = v; return x;
}
Value Value::text(std::string v) {
  Value x; x.kind_ = Kind::String; x.string_ = std::move(v); return x;
}
Value Value::vec(std::vector<double> v) {
  Value x; x.kind_ = Kind::FloatVec; x.vec_ = std::move(v); return x;
}

const Param* findParam(const OperatorDesc& op, const std::string& name) {
  for (const auto& p : op.params) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const char* toString(ParamType t) {
  switch (t) {
    case ParamType::Bool:      return "bool";
    case ParamType::Int:       return "int";
    case ParamType::Float:     return "float";
    case ParamType::Vec2f:     return "vec2f";
    case ParamType::Vec3f:     return "vec3f";
    case ParamType::Vec4f:     return "vec4f";
    case ParamType::Enum:      return "enum";
    case ParamType::Flags:     return "flags";
    case ParamType::String:    return "string";
    case ParamType::Text:      return "text";
    case ParamType::Path:      return "path";
    case ParamType::Color:     return "color";
    case ParamType::Transform: return "transform";
    case ParamType::Curve:     return "curve";
  }
  return "float";
}

bool parseParamType(const std::string& name, ParamType& out) {
  static const ParamType kAll[] = {
      ParamType::Bool,   ParamType::Int,    ParamType::Float,     ParamType::Vec2f,
      ParamType::Vec3f,  ParamType::Vec4f,  ParamType::Enum,      ParamType::Flags,
      ParamType::String, ParamType::Text,   ParamType::Path,      ParamType::Color,
      ParamType::Transform, ParamType::Curve,
  };
  for (ParamType t : kAll) {
    if (name == toString(t)) {
      out = t;
      return true;
    }
  }
  return false;
}

Port withContract(Port p, nlohmann::json contract) {
  p.contract = std::move(contract);
  return p;
}

Port withExample(Port p, nlohmann::json example) {
  p.example = std::move(example);
  return p;
}

SnippetDesc parseSnippet(const std::string& text, const std::string& source) {
  SnippetDesc s;
  s.source = source;
  nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    s.parseError = "不是合法的 JSON 对象";
    s.id = source;
    return s;
  }
  auto text_of = [&](const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  s.id = text_of("id");
  s.label = text_of("label");
  s.category = text_of("category");
  s.doc = text_of("doc");
  s.body = std::move(j);
  return s;
}

// ------------------------------------------------------------ curve 的值

bool checkCurveValue(const nlohmann::json& j, const Param& p, std::string& message) {
  const auto num = [](double d) { return nlohmann::json(d).dump(); };
  if (!j.is_object()) {
    message = "应当是 {\"points\": [[x, y], …], \"interp\": …} 这样的对象";
    return false;
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key() != "points" && it.key() != "interp") {
      message = "不认识的字段 '" + it.key() + "'（只有 points 与 interp）";
      return false;
    }
  }
  auto interp = j.find("interp");
  if (interp != j.end() &&
      !(interp->is_string() && (*interp == "linear" || *interp == "smooth"))) {
    message = "interp 只能是 \"linear\" 或 \"smooth\"";
    return false;
  }
  auto points = j.find("points");
  if (points == j.end() || !points->is_array() || points->size() < 2) {
    message = "points 至少要有两个控制点";
    return false;
  }
  double prevX = 0;
  for (std::size_t i = 0; i < points->size(); ++i) {
    const auto& pt = (*points)[i];
    const std::string at = "第 " + std::to_string(i + 1) + " 个控制点";
    if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number()) {
      message = at + "应当是 [x, y] 两个数";
      return false;
    }
    const double x = pt[0].get<double>();
    const double y = pt[1].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y)) {
      message = at + "必须是有限的数字";
      return false;
    }
    if (x < 0 || x > 1) {
      message = at + "的 x 必须在 [0, 1] 内，实际是 " + num(x);
      return false;
    }
    if (i > 0 && !(x > prevX)) {
      message = at + "的 x 必须大于前一个点（" + num(prevX) + "）";
      return false;
    }
    if (p.min && y < *p.min) {
      message = at + "的 y 不能小于 " + num(*p.min);
      return false;
    }
    if (p.max && y > *p.max) {
      message = at + "的 y 不能大于 " + num(*p.max);
      return false;
    }
    prevX = x;
  }
  return true;
}

double evaluateCurve(const nlohmann::json& curve, double x) {
  Param any;
  std::string ignored;
  if (!checkCurveValue(curve, any, ignored)) return 0.0;
  const auto& pts = curve["points"];
  const std::size_t n = pts.size();
  std::vector<double> xs(n), ys(n);
  for (std::size_t i = 0; i < n; ++i) {
    xs[i] = pts[i][0].get<double>();
    ys[i] = pts[i][1].get<double>();
  }
  if (x <= xs.front()) return ys.front();
  if (x >= xs.back()) return ys.back();
  std::size_t k = 0;
  while (k + 1 < n && x > xs[k + 1]) ++k;
  const double h = xs[k + 1] - xs[k];
  const double t = (x - xs[k]) / h;
  const bool smooth = curve.value("interp", std::string("linear")) == "smooth";
  if (!smooth) return ys[k] + (ys[k + 1] - ys[k]) * t;

  // Fritsch–Carlson：先取各段割线斜率，端点用单侧差分，内点符号相反或有一段水平时取 0，
  // 再把 (α, β) 压进半径 3 的圆里 —— 这样插出来的线在每一段上单调、不冲过端点的 y。
  std::vector<double> delta(n - 1), m(n);
  for (std::size_t i = 0; i + 1 < n; ++i) delta[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
  m[0] = delta[0];
  m[n - 1] = delta[n - 2];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    m[i] = delta[i - 1] * delta[i] <= 0 ? 0.0 : (delta[i - 1] + delta[i]) / 2;
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (delta[i] == 0) {
      m[i] = 0;
      m[i + 1] = 0;
      continue;
    }
    const double a = m[i] / delta[i];
    const double b = m[i + 1] / delta[i];
    const double s = a * a + b * b;
    if (s > 9) {
      const double tau = 3 / std::sqrt(s);
      m[i] = tau * a * delta[i];
      m[i + 1] = tau * b * delta[i];
    }
  }
  const double t2 = t * t;
  const double t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * m[k] +
         (-2 * t3 + 3 * t2) * ys[k + 1] + (t3 - t2) * h * m[k + 1];
}

}  // namespace lyflow
