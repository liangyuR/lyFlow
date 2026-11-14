#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/value.h"

namespace lyflow::packs::std_value {
namespace {

using Json = nlohmann::json;

Status bad(const std::string& message, const std::string& port = {}) {
  return Status::Error(Phase::Execute, "bad_input", message, {}, port);
}

Status put(Outputs& outputs, Record record, const char* port = "value") {
  std::string why;
  if (!values::validate(record, why))
    return bad(why);
  outputs.set(port, Data::record(std::move(record)));
  return Status::Ok();
}

Param text(const char* name, const char* label, const char* def = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
}

Param number(const char* name, const char* label, double def = 0) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(def);
  return p;
}

Param integer(const char* name, const char* label, std::int64_t def = 0) {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Int;
  p.def = Value::integer(def);
  return p;
}

Param choice(const char* name, const char* label, const char* def,
             std::initializer_list<EnumOption> options) {
  Param p = text(name, label, def);
  p.type = ParamType::Enum;
  p.options = options;
  return p;
}

Port valuePort(const char* name, const char* label, bool required = true) {
  return withContract(Port{name, "Record", label, "std.Value.v1", required},
                      Json{{"recordType", values::kValueType}});
}

Port listPort(const char* name, const char* label) {
  return withContract(Port{name, "Record", label, "std.List.v1", true},
                      Json{{"recordType", values::kListType}});
}

OperatorDesc op(const char* id, const char* label, const char* doc, ComputeFn compute) {
  OperatorDesc result;
  result.id = id;
  result.version = "1.0.0";
  result.label = label;
  result.category = "通用/值与集合";
  result.doc = doc;
  result.compute = std::move(compute);
  result.capabilities = {false, true, true};
  return result;
}

bool numeric(const Data& input, const Record*& record, std::string& why) {
  if (!values::readValue(input, record, why))
    return false;
  if (record->data["kind"] != "number") {
    why = "输入必须为 number 值";
    return false;
  }
  return true;
}

bool compatible(const Record& a, const Record& b) {
  return a.data["kind"] == b.data["kind"] && a.data["unit"] == b.data["unit"];
}

bool parseArray(const std::string& source, Json& array, std::string& why) {
  if (source.size() > values::kMaxBytes) {
    why = "JSON 文本超过 1 MiB";
    return false;
  }
  try {
    array = Json::parse(source, [](int depth, Json::parse_event_t, Json&) {
      if (depth > static_cast<int>(values::kMaxDepth))
        throw std::runtime_error("JSON 嵌套深度超过 16");
      return true;
    });
  } catch (const std::exception&) {
    why = "JSON 数组格式错误或超过深度限制";
    return false;
  }
  if (!array.is_array() || array.size() > values::kMaxItems) {
    why = "需要最多 4096 项的 JSON 数组";
    return false;
  }
  return true;
}

Status arithmetic(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record *a = nullptr, *b = nullptr;
  std::string why;
  if (!numeric(in.get("a"), a, why))
    return bad(why, "a");
  if (!numeric(in.get("b"), b, why))
    return bad(why, "b");
  const double av = a->data["value"].get<double>(), bv = b->data["value"].get<double>();
  const std::string au = a->data["unit"], bu = b->data["unit"];
  const auto& operation = p.choice("operation");
  std::string unit;
  double result = 0;
  if (operation == "add" || operation == "subtract") {
    if (au != bu)
      return bad("加减运算的单位必须一致");
    unit = au;
    result = operation == "add" ? av + bv : av - bv;
  } else if (operation == "multiply") {
    if (!au.empty() && !bu.empty())
      return bad("乘法仅支持至少一端无单位；复合量纲需显式换算");
    unit = au.empty() ? bu : au;
    result = av * bv;
  } else if (operation == "divide") {
    if (bv == 0)
      return bad("除数不能为零", "b");
    if (!bu.empty() && au != bu)
      return bad("除法仅支持无单位除数或相同单位相消");
    unit = bu.empty() ? au : "";
    result = av / bv;
  } else {
    if (!au.empty() || !bu.empty())
      return bad("幂运算仅支持无单位底数和指数");
    if (av == 0 && bv <= 0)
      return bad("零的零次幂或负次幂没有有效定义");
    result = std::pow(av, bv);
  }
  if (!std::isfinite(result))
    return bad("算术结果溢出或不是有限数");
  return put(out, values::number(result, unit));
}

Status compare(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record *a = nullptr, *b = nullptr;
  std::string why;
  if (!values::readValue(in.get("a"), a, why))
    return bad(why, "a");
  if (!values::readValue(in.get("b"), b, why))
    return bad(why, "b");
  if (!compatible(*a, *b))
    return bad("比较两端的类型和单位必须一致");
  const auto& operation = p.choice("operation");
  const double absTol = p.number("absoluteTolerance"), relTol = p.number("relativeTolerance");
  if (!std::isfinite(absTol) || !std::isfinite(relTol) || absTol < 0 || relTol < 0)
    return bad("容差必须是非负有限数");
  bool result = false;
  if (a->data["kind"] == "number") {
    const long double av = a->data["value"].get<double>();
    const long double bv = b->data["value"].get<double>();
    const long double scale = std::max(std::abs(av), std::abs(bv));
    const long double distance = std::abs(av - bv);
    const bool equal =
        scale == 0 || distance <= absTol ||
        (std::isfinite(distance) ? (distance - absTol) / scale <= relTol
                                 : std::abs(av / scale - bv / scale) <= absTol / scale + relTol);
    result = operation == "equal"        ? equal
             : operation == "not_equal"  ? !equal
             : operation == "less"       ? av < bv && !equal
             : operation == "less_equal" ? av < bv || equal
             : operation == "greater"    ? av > bv && !equal
                                         : av > bv || equal;
  } else {
    if (a->data["kind"] == "record")
      return bad("记录对象不支持比较，请先提取具体字段");
    if (operation != "equal" && operation != "not_equal")
      return bad("布尔和字符串仅支持相等/不等比较");
    const bool equal = a->data["value"] == b->data["value"];
    result = operation == "equal" ? equal : !equal;
  }
  return put(out, values::boolean(result));
}

Status logic(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record *a = nullptr, *b = nullptr;
  std::string why;
  if (!values::readValue(in.get("a"), a, why) || a->data["kind"] != "boolean")
    return bad("a 必须是 boolean 值", "a");
  const bool av = a->data["value"].get<bool>();
  const auto& operation = p.choice("operation");
  if (operation == "not")
    return put(out, values::boolean(!av));
  if (!in.has("b") || !values::readValue(in.get("b"), b, why) || b->data["kind"] != "boolean")
    return bad("二元逻辑运算需要 boolean 输入 b", "b");
  const bool bv = b->data["value"].get<bool>();
  const bool result = operation == "and"    ? av && bv
                      : operation == "or"   ? av || bv
                      : operation == "xor"  ? av != bv
                      : operation == "nand" ? !(av && bv)
                      : operation == "nor"  ? !(av || bv)
                                            : av == bv;
  return put(out, values::boolean(result));
}

Status field(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const auto* record = in.get("record").asRecord();
  if (!record)
    return bad("需要 Record 输入", "record");
  std::string why;
  const Json* object = &record->data;
  if (record->type == values::kValueType) {
    if (!values::validate(*record, why))
      return bad(why, "record");
    if (record->data["kind"] == "record")
      object = &record->data["value"]["data"];
  }
  if (!values::boundedJson(*object, why, true))
    return bad(why, "record");
  const auto& pointer = p.text("pointer");
  if (pointer.size() > values::kMaxStringBytes)
    return bad("字段路径过长");
  const Json* selected = nullptr;
  try {
    selected = &object->at(Json::json_pointer(pointer));
  } catch (const Json::exception&) {
    return bad("字段路径不存在或不是合法 JSON Pointer", "record");
  }
  if (selected->is_number())
    return put(out, values::number(selected->get<double>(), p.text("unit")));
  if (!p.text("unit").empty())
    return bad("非数值字段不能声明单位");
  if (selected->is_boolean())
    return put(out, values::boolean(selected->get<bool>()));
  if (selected->is_string())
    return put(out, values::string(selected->get<std::string>()));
  return bad("value.field 只提取 number/boolean/string 字段；集合使用 list 算子");
}

Status pack(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const Record *a = nullptr, *b = nullptr;
  std::string why;
  if (!values::readValue(in.get("a"), a, why))
    return bad(why, "a");
  Json items = Json::array({a->data["value"]});
  if (in.has("b")) {
    if (!values::readValue(in.get("b"), b, why))
      return bad(why, "b");
    if (!compatible(*a, *b))
      return bad("列表元素的 kind 和 unit 必须一致");
    items.push_back(b->data["value"]);
  }
  return put(out, values::makeList(a->data["kind"], std::move(items), a->data["unit"]), "list");
}

Status concat(const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
  const Record *a = nullptr, *b = nullptr;
  std::string why;
  if (!values::readList(in.get("a"), a, why))
    return bad(why, "a");
  if (!values::readList(in.get("b"), b, why))
    return bad(why, "b");
  if (!compatible(*a, *b))
    return bad("拼接列表的 kind 和 unit 必须一致");
  if (a->data["items"].size() + b->data["items"].size() > values::kMaxItems)
    return bad("拼接后超过 4096 项");
  Json items = a->data["items"];
  for (const auto& item : b->data["items"])
    items.push_back(item);
  return put(out, values::makeList(a->data["kind"], std::move(items), a->data["unit"]), "list");
}

Status get(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record* list = nullptr;
  std::string why;
  if (!values::readList(in.get("list"), list, why))
    return bad(why, "list");
  const auto index = p.integer("index");
  const auto& items = list->data["items"];
  if (index < 0 || static_cast<std::uint64_t>(index) >= items.size())
    return bad("索引越界；只接受从 0 开始的非负索引");
  return put(out, values::makeValue(list->data["kind"], items[static_cast<std::size_t>(index)],
                                    list->data["unit"]));
}

Status subset(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record* list = nullptr;
  std::string why;
  if (!values::readList(in.get("list"), list, why))
    return bad(why, "list");
  Json indices;
  if (!parseArray(p.text("indices"), indices, why))
    return bad(why);
  Json items = Json::array();
  const auto& source = list->data["items"];
  for (const auto& index : indices) {
    if ((!index.is_number_integer() && !index.is_number_unsigned()) ||
        (index.is_number_integer() && index.get<std::int64_t>() < 0) ||
        index.get<std::uint64_t>() >= source.size())
      return bad("子集索引必须是范围内的非负整数");
    items.push_back(source[index.get<std::size_t>()]);
  }
  return put(out, values::makeList(list->data["kind"], std::move(items), list->data["unit"]),
             "list");
}

Status sort(const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
  const Record* list = nullptr;
  std::string why;
  if (!values::readList(in.get("list"), list, why))
    return bad(why, "list");
  const auto& kind = list->data["kind"].get_ref<const std::string&>();
  const auto& items = list->data["items"];
  Json::json_pointer pointer;
  try {
    pointer = Json::json_pointer(p.text("pointer"));
  } catch (const Json::exception&) {
    return bad("排序字段不是合法 JSON Pointer");
  }
  std::vector<Json> keys;
  for (const auto& item : items) {
    const Json* key = &item;
    if (kind == "record") {
      try {
        key = &item["data"].at(pointer);
      } catch (const Json::exception&) {
        return bad("记录缺少排序字段");
      }
    } else if (!p.text("pointer").empty()) {
      return bad("标量排序不能使用字段路径");
    }
    if (!key->is_number() && !key->is_string() && !key->is_boolean())
      return bad("排序字段必须是数值、字符串或布尔");
    if (key->is_number() && !values::safeNumber(*key))
      return bad("数值排序键必须是有限 float64，整数不得超过 [-2^53,2^53]");
    const auto sameType = [](const Json& a, const Json& b) {
      return (a.is_number() && b.is_number()) || a.type() == b.type();
    };
    if (!keys.empty() && !sameType(keys.front(), *key))
      return bad("全部排序字段必须同类型");
    keys.push_back(*key);
  }
  std::vector<std::size_t> order(items.size());
  std::iota(order.begin(), order.end(), 0);
  const bool descending = p.choice("direction") == "descending";
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const Json& first = descending ? keys[b] : keys[a];
    const Json& second = descending ? keys[a] : keys[b];
    if (first.is_number())
      return first.get<double>() < second.get<double>();
    if (first.is_boolean())
      return first.get<bool>() < second.get<bool>();
    return first.get_ref<const std::string&>() < second.get_ref<const std::string&>();
  });
  Json result = Json::array(), permutation = Json::array();
  for (const auto index : order) {
    result.push_back(items[index]);
    permutation.push_back(index);
  }
  auto status = put(out, values::makeList(kind, std::move(result), list->data["unit"]), "list");
  if (!status.ok)
    return status;
  return put(out, values::makeList("number", std::move(permutation)), "indices");
}

} // namespace

void registerPackOps(Registry& r) {
  {
    auto d = op("value.number", "数值常量", "可连接的有限数值，单位必须显式声明。",
                [](const Inputs&, const ParamView& p, Outputs& out, ExecContext&) {
                  return put(out, values::number(p.number("value"), p.text("unit")));
                });
    d.keywords = {"number", "constant", "numeric input", "数字", "数值输入", "常数"};
    d.params = {number("value", "数值"), text("unit", "单位")};
    d.outputs = {valuePort("value", "数值")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.boolean", "布尔常量", "独立布尔值，不使用 Measurement.ok 表示普通数字。",
                [](const Inputs&, const ParamView& p, Outputs& out, ExecContext&) {
                  return put(out, values::boolean(p.flag("value")));
                });
    Param p;
    p.name = "value";
    p.label = "布尔值";
    p.type = ParamType::Bool;
    p.def = Value::boolean(false);
    d.params = {p};
    d.keywords = {"boolean", "bool", "true false", "布尔", "逻辑常量"};
    d.outputs = {valuePort("value", "布尔值")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.string", "字符串常量", "UTF-8 文本常量，最多 64 KiB。",
                [](const Inputs&, const ParamView& p, Outputs& out, ExecContext&) {
                  return put(out, values::string(p.text("value")));
                });
    d.params = {text("value", "文本")};
    d.keywords = {"string", "text", "info", "字符串", "文本", "输入信息"};
    d.outputs = {valuePort("value", "文本")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.from_measurement", "测量值转数值", "仅接受有效且有限的测量值，保留单位。",
                [](const Inputs& in, const ParamView&, Outputs& out, ExecContext&) {
                  const auto* m = in.get("measurement").asMeasurement();
                  if (!m || !m->ok || !std::isfinite(m->value))
                    return bad("测量无效，不能转换成数值", "measurement");
                  return put(out, values::number(m->value, m->unit));
                });
    d.keywords = {"measurement", "convert", "测量", "测量值转换"};
    d.inputs = {Port{"measurement", "Measurement", "测量值", "必须有效且有限。", true}};
    d.outputs = {valuePort("value", "数值")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.field", "记录字段提取",
                "通过 JSON Pointer 提取标量；数字单位由参数显式指定。", &field);
    d.keywords = {"record", "field", "JSON pointer", "记录", "字段", "信息提取"};
    d.inputs = {Port{"record", "Record", "记录", "领域 Record 或记录值包装。", true}};
    d.outputs = {valuePort("value", "字段值")};
    d.params = {text("pointer", "字段路径", "/value"), text("unit", "数值单位")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.arithmetic", "数值算术运算",
                "有限数四则与无单位幂运算；加减同单位，乘除只支持明确的标量量纲。", &arithmetic);
    d.keywords = {"arithmetic", "add",  "subtract", "multiply", "divide",
                  "power",      "四则", "加减乘除", "幂",       "算术"};
    d.inputs = {valuePort("a", "A"), valuePort("b", "B")};
    d.outputs = {valuePort("value", "结果")};
    d.params = {choice("operation", "运算", "add",
                       {{"add", "加", ""},
                        {"subtract", "减", ""},
                        {"multiply", "乘", ""},
                        {"divide", "除", ""},
                        {"power", "幂", ""}})};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.compare", "数值与标量比较",
                "数值比较支持绝对/相对容差；布尔/文本只支持相等与不等。", &compare);
    d.keywords = {"compare", "comparison", "tolerance", "threshold", "比较", "容差", "阈值"};
    d.inputs = {valuePort("a", "A"), valuePort("b", "B")};
    d.outputs = {valuePort("value", "布尔结果")};
    auto absTol = number("absoluteTolerance", "绝对容差", 1e-9);
    auto relTol = number("relativeTolerance", "相对容差", 1e-9);
    absTol.min = relTol.min = 0;
    d.params = {choice("operation", "比较", "equal",
                       {{"equal", "等于", ""},
                        {"not_equal", "不等于", ""},
                        {"less", "小于", ""},
                        {"less_equal", "小于等于", ""},
                        {"greater", "大于", ""},
                        {"greater_equal", "大于等于", ""}}),
                absTol, relTol};
    r.addOperator(std::move(d));
  }
  {
    auto d =
        op("value.logic", "布尔逻辑运算",
           "与/或/异或/非/与非/或非/同或。二元输入正常求值；惰性分支使用 flow.select(path=value)。",
           &logic);
    d.keywords = {"logic", "boolean", "and", "or", "xor", "not", "逻辑", "与或非", "异或"};
    d.inputs = {valuePort("a", "A"), valuePort("b", "B", false)};
    d.outputs = {valuePort("value", "布尔结果")};
    d.params = {choice("operation", "运算", "and",
                       {{"and", "与", ""},
                        {"or", "或", ""},
                        {"xor", "异或", ""},
                        {"not", "非", ""},
                        {"nand", "与非", ""},
                        {"nor", "或非", ""},
                        {"xnor", "同或", ""}})};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("value.concat", "字符串拼接",
                "按 A、分隔符、B 顺序拼接，保留空串与 Unicode，最多64 KiB。",
                [](const Inputs& in, const ParamView& p, Outputs& out, ExecContext&) {
                  const Record *a = nullptr, *b = nullptr;
                  std::string why;
                  if (!values::readValue(in.get("a"), a, why) || a->data["kind"] != "string")
                    return bad("a 必须是 string 值", "a");
                  if (!values::readValue(in.get("b"), b, why) || b->data["kind"] != "string")
                    return bad("b 必须是 string 值", "b");
                  const auto& av = a->data["value"].get_ref<const std::string&>();
                  const auto& bv = b->data["value"].get_ref<const std::string&>();
                  const auto& connector = p.text("connector");
                  if (connector.size() > values::kMaxStringBytes ||
                      av.size() + bv.size() + connector.size() > values::kMaxStringBytes)
                    return bad("拼接后的字符串超过 64 KiB");
                  return put(out, values::string(av + connector + bv));
                });
    d.keywords = {"string", "concat", "concatenate", "join", "字符串拼接", "连接符"};
    d.inputs = {valuePort("a", "A"), valuePort("b", "B")};
    d.outputs = {valuePort("value", "文本")};
    d.params = {text("connector", "分隔符")};
    r.addOperator(std::move(d));
  }
  {
    auto d =
        op("list.constant", "列表常量",
           "同类型有界列表。最多 4096 项，不接受 null、非有限数和大数组。",
           [](const Inputs&, const ParamView& p, Outputs& out, ExecContext&) {
             Json items;
             std::string why;
             if (!parseArray(p.text("items"), items, why))
               return bad(why);
             return put(out, values::makeList(p.choice("kind"), std::move(items), p.text("unit")),
                        "list");
           });
    d.keywords = {"list", "vector", "array", "constant", "列表", "数组", "集合常量"};
    auto items = text("items", "JSON 元素数组", "[]");
    items.type = ParamType::Text;
    items.rows = 4;
    d.params = {choice("kind", "元素类型", "number",
                       {{"number", "数值", ""},
                        {"boolean", "布尔", ""},
                        {"string", "字符串", ""},
                        {"record", "记录", ""}}),
                items, text("unit", "数值单位")};
    d.outputs = {listPort("list", "列表")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("list.pack", "值合并为列表",
                "将一个或两个同类型值按端口顺序合并；不去重、不扁平化。", &pack);
    d.keywords = {"merge vector", "pack", "list", "合并集合", "合并列表", "值列表"};
    d.inputs = {valuePort("a", "A"), valuePort("b", "B", false)};
    d.outputs = {listPort("list", "列表")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("list.concat", "列表拼接", "按 A 后 B 顺序拼接，保留重复项和记录身份，不扁平化。",
                &concat);
    d.keywords = {"concat", "concatenate", "vector", "append", "列表拼接", "数组拼接"};
    d.inputs = {listPort("a", "A"), listPort("b", "B")};
    d.outputs = {listPort("list", "列表")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("list.get", "列表元素提取", "从 0 开始的非负索引，越界产生诊断。", &get);
    d.keywords = {"fetch", "get", "element", "index", "取元素", "索引", "列表提取"};
    d.inputs = {listPort("list", "列表")};
    d.outputs = {valuePort("value", "元素")};
    auto index = integer("index", "索引");
    index.min = 0;
    d.params = {index};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("list.subset", "列表子集提取",
                "按显式非负索引顺序提取，允许重复项，空索引得到空列表。", &subset);
    d.keywords = {"subset", "indices", "vector", "列表子集", "数组子集", "索引筛选"};
    d.inputs = {listPort("list", "列表")};
    d.outputs = {listPort("list", "列表")};
    d.params = {text("indices", "JSON 索引数组", "[]")};
    r.addOperator(std::move(d));
  }
  {
    auto d = op("list.stable_sort", "列表稳定排序",
                "按标量或记录字段排序；相同键保留输入顺序，输出原始索引。", &sort);
    d.keywords = {"stable sort", "order", "vector", "稳定排序", "列表排序"};
    d.inputs = {listPort("list", "列表")};
    d.outputs = {listPort("list", "排序列表"), listPort("indices", "原始索引")};
    d.params = {text("pointer", "记录字段路径"),
                choice("direction", "方向", "ascending",
                       {{"ascending", "升序", ""}, {"descending", "降序", ""}})};
    r.addOperator(std::move(d));
  }
}

} // namespace lyflow::packs::std_value
