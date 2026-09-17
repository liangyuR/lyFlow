#include "lyflow/contract.h"

#include <algorithm>
#include <cmath>

namespace lyflow {
namespace {

bool isKnownKey(const std::string& key) {
  for (const char* k : kPortContractKeys) {
    if (key == k) return true;
  }
  return false;
}

std::string knownKeyList() {
  std::string out;
  for (const char* k : kPortContractKeys) {
    if (!out.empty()) out += " / ";
    out += k;
  }
  return out;
}

/// 非负整数。JSON 里 1280.0 与 1280 在 nlohmann 下是两种类型，所以只认整数 ——
/// 「点数等于 1279.5」没有意义，写错了要当场知道。
bool isCount(const nlohmann::json& j) {
  return j.is_number_integer() && j.get<std::int64_t>() >= 0;
}

/// 点云的坐标全有限。normals/intensity 不看 —— 契约说的是「这一片云能不能算」，
/// 而所有几何算子读的都是 xyz。
bool cloudFinite(const PointCloud& c) {
  for (float v : c.xyz) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

/// 非有限元素的个数，最多数到 limit 就够报错用了（actual 里只报个数）。
std::size_t countNonFinite(const std::vector<float>& v) {
  std::size_t n = 0;
  for (float x : v) {
    if (!std::isfinite(x)) n += 1;
  }
  return n;
}

}  // namespace

std::vector<std::string> validatePortContract(const nlohmann::json& contract,
                                              const std::string& where) {
  std::vector<std::string> problems;
  auto fail = [&](const std::string& msg) { problems.push_back(where + " contract: " + msg); };

  if (contract.is_null()) return problems;
  if (!contract.is_object()) {
    fail("必须是一个 JSON 对象");
    return problems;
  }
  if (contract.empty()) return problems;

  for (auto it = contract.begin(); it != contract.end(); ++it) {
    if (!isKnownKey(it.key())) {
      // 「再加一种键」的门槛就在这一行。四种之外的任何东西都要先改 ADR-0024。
      fail("不认识的键 '" + it.key() + "'，只有四种：" + knownKeyList());
    }
  }

  auto ec = contract.find("elementCount");
  if (ec != contract.end()) {
    if (!ec->is_object() || ec->empty()) {
      fail("elementCount 要写成 { eq | min | max }");
    } else {
      for (auto it = ec->begin(); it != ec->end(); ++it) {
        if (it.key() != "eq" && it.key() != "min" && it.key() != "max") {
          fail("elementCount 里不认识的键 '" + it.key() + "'，只有 eq / min / max");
        } else if (!isCount(*it)) {
          fail("elementCount." + it.key() + " 要是一个非负整数");
        }
      }
      const auto min = ec->find("min");
      const auto max = ec->find("max");
      if (min != ec->end() && max != ec->end() && isCount(*min) && isCount(*max) &&
          min->get<std::int64_t>() > max->get<std::int64_t>()) {
        fail("elementCount.min 大于 max");
      }
      if (ec->contains("eq") && (min != ec->end() || max != ec->end())) {
        fail("elementCount.eq 与 min/max 只能选一种写法");
      }
    }
  }

  auto fin = contract.find("finite");
  if (fin != contract.end() && !(fin->is_boolean() && fin->get<bool>())) {
    // 只允许 true：`finite: false` 读起来像「这里允许 NaN」，而那是**没有契约**，
    // 两种写法表达同一件事必然有人写错一种。
    fail("finite 只能写 true（不需要这条就整个别写）");
  }

  auto shape = contract.find("shape");
  if (shape != contract.end()) {
    if (!shape->is_array() || shape->empty()) {
      fail("shape 要是一个非空整数数组");
    } else {
      for (const auto& d : *shape) {
        if (!d.is_number_integer() || d.get<std::int64_t>() < -1) {
          fail("shape 的每一维要是 >= 0 的整数，或 -1 表示任意");
        }
      }
    }
  }

  auto rt = contract.find("recordType");
  if (rt != contract.end() && !(rt->is_string() && !rt->get<std::string>().empty())) {
    fail("recordType 要是一个非空字串");
  }
  return problems;
}

bool checkPortContract(const nlohmann::json& contract, const Data& value,
                       nlohmann::json& expected, nlohmann::json& actual, std::string& message) {
  // 没声明契约的端口：这里立刻返回，连 value 都不碰（默认零开销）。
  if (!contract.is_object() || contract.empty()) return true;

  auto reject = [&](nlohmann::json want, nlohmann::json got, std::string why) {
    expected = std::move(want);
    actual = std::move(got);
    message = std::move(why);
    return false;
  };

  auto ec = contract.find("elementCount");
  if (ec != contract.end() && ec->is_object()) {
    const auto n = static_cast<std::int64_t>(value.elementCount());
    auto eq = ec->find("eq");
    if (eq != ec->end() && isCount(*eq) && n != eq->get<std::int64_t>()) {
      return reject({{"elementCount", *ec}}, {{"elementCount", n}},
                    "端口契约：元素数应当是 " + std::to_string(eq->get<std::int64_t>()) +
                        "，实际是 " + std::to_string(n));
    }
    auto min = ec->find("min");
    if (min != ec->end() && isCount(*min) && n < min->get<std::int64_t>()) {
      return reject({{"elementCount", *ec}}, {{"elementCount", n}},
                    "端口契约：元素数不能少于 " + std::to_string(min->get<std::int64_t>()) +
                        "，实际是 " + std::to_string(n));
    }
    auto max = ec->find("max");
    if (max != ec->end() && isCount(*max) && n > max->get<std::int64_t>()) {
      return reject({{"elementCount", *ec}}, {{"elementCount", n}},
                    "端口契约：元素数不能多于 " + std::to_string(max->get<std::int64_t>()) +
                        "，实际是 " + std::to_string(n));
    }
  }

  auto shape = contract.find("shape");
  if (shape != contract.end() && shape->is_array()) {
    const Tensor* t = value.asTensor();
    if (t == nullptr) {
      return reject({{"shape", *shape}}, {{"type", std::string(value.typeName())}},
                    "端口契约：要的是张量，实际收到 " + std::string(value.typeName()));
    }
    nlohmann::json got = t->shape;
    bool okShape = t->shape.size() == shape->size();
    for (std::size_t i = 0; okShape && i < shape->size(); ++i) {
      const auto want = (*shape)[i].is_number_integer() ? (*shape)[i].get<std::int64_t>() : -1;
      if (want >= 0 && t->shape[i] != want) okShape = false;
    }
    if (!okShape) {
      return reject({{"shape", *shape}}, {{"shape", got}},
                    "端口契约：形状应当是 " + shape->dump() + "（-1 表示任意），实际是 " +
                        got.dump());
    }
  }

  auto rt = contract.find("recordType");
  if (rt != contract.end() && rt->is_string()) {
    const Record* r = value.asRecord();
    const std::string want = rt->get<std::string>();
    if (r == nullptr) {
      return reject({{"recordType", want}}, {{"type", std::string(value.typeName())}},
                    "端口契约：要的是 Record（" + want + "），实际收到 " +
                        std::string(value.typeName()));
    }
    if (r->type != want) {
      return reject({{"recordType", want}}, {{"recordType", r->type}},
                    "端口契约：Record 的类型应当是 " + want + "，实际是 " + r->type);
    }
  }

  auto fin = contract.find("finite");
  if (fin != contract.end() && fin->is_boolean() && fin->get<bool>()) {
    // O(n) 一遍，只在声明了 finite 的端口上跑（m6-plan §9）。
    if (const PointCloud* c = value.asCloud()) {
      if (!cloudFinite(*c)) {
        const std::size_t bad = countNonFinite(c->xyz) ;
        return reject({{"finite", true}}, {{"nonFiniteCoords", bad}},
                      "端口契约：坐标必须全是有限值，实际有 " + std::to_string(bad) +
                          " 个非有限分量（共 " + std::to_string(c->xyz.size()) + " 个）");
      }
    } else if (const Tensor* t = value.asTensor()) {
      const std::size_t bad = countNonFinite(t->data);
      if (bad != 0) {
        return reject({{"finite", true}}, {{"nonFinite", bad}},
                      "端口契约：张量必须全是有限值，实际有 " + std::to_string(bad) +
                          " 个非有限元素（共 " + std::to_string(t->data.size()) + " 个）");
      }
    } else if (const Measurement* m = value.asMeasurement()) {
      if (!std::isfinite(m->value)) {
        return reject({{"finite", true}}, {{"value", nullptr}},
                      "端口契约：测量值必须是有限的，实际没测出来（" + m->message + "）");
      }
    }
    // 其余类型上 finite 无从谈起：这在 Registry::validate() 里就被拦掉了，
    // 走到这里说明那一层漏了，宁可放行也不要在执行期造一个假失败。
  }
  return true;
}

}  // namespace lyflow
