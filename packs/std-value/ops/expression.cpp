#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "lyflow/operator.h"
#include "lyflow/registry.h"
#include "lyflow/value.h"

namespace lyflow::packs::std_value {
namespace {

constexpr std::size_t kMaxSource = 4096;
constexpr std::size_t kMaxNodes = 1024;
constexpr std::size_t kMaxDepth = 64;

struct Failure {
  Status status;
};

[[noreturn]] void syntax(std::size_t position, const std::string& message) {
  throw Failure{Status::Error(Phase::Execute, "bad_param",
                              "表达式第 " + std::to_string(position + 1) + " 字节：" + message,
                              "expression")};
}

[[noreturn]] void invalid(std::size_t position, const std::string& message,
                          const std::string& port = {}) {
  throw Failure{Status::Error(Phase::Execute, "bad_input",
                              "表达式第 " + std::to_string(position + 1) + " 字节：" + message, {},
                              port)};
}

void checkCancelled(ExecContext* context) {
  if (context && context->cancelled())
    throw Failure{Status::Error(Phase::Execute, "cancelled", "表达式计算已取消")};
}

bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool blank(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

enum class TokenKind { End, Number, Identifier, Left, Right, Comma, Question, Colon, Operator };
struct Token {
  TokenKind kind = TokenKind::End;
  std::size_t position = 0;
  std::string text;
  double number = 0;
};

enum class Kind { Number, Boolean, Variable, Unary, Binary, Conditional, Function };
struct Node {
  Kind kind = Kind::Number;
  std::size_t position = 0;
  std::string text;
  double number = 0;
  bool boolean = false;
  std::array<int, 3> children{{-1, -1, -1}};
  std::size_t count = 0;
  std::size_t depth = 1;
};

struct Program {
  std::vector<Node> nodes;
  int root = -1;
};

class Parser {
public:
  Parser(const std::string& source, ExecContext* context) : source_(source), context_(context) {}

  Program parse() {
    checkCancelled(context_);
    if (source_.size() > kMaxSource)
      syntax(kMaxSource, "文本不能超过 4096 字节");
    next();
    const int root = conditional(0);
    if (token_.kind != TokenKind::End)
      syntax(token_.position, "预期表达式结束");
    return {std::move(nodes_), root};
  }

private:
  void next() {
    checkCancelled(context_);
    while (cursor_ < source_.size() && blank(source_[cursor_]))
      ++cursor_;
    token_ = {};
    token_.position = cursor_;
    if (cursor_ == source_.size())
      return;
    const std::size_t start = cursor_;
    const char c = source_[cursor_++];
    if (digit(c) || c == '.') {
      std::size_t digits = digit(c) ? 1 : 0;
      if (c != '.') {
        while (cursor_ < source_.size() && digit(source_[cursor_])) {
          ++digits;
          ++cursor_;
        }
        if (cursor_ < source_.size() && source_[cursor_] == '.')
          ++cursor_;
      }
      while (cursor_ < source_.size() && digit(source_[cursor_])) {
        ++digits;
        ++cursor_;
      }
      if (c == '.' && digits == 0)
        syntax(start, "小数点需要数字");
      if (cursor_ < source_.size() && (source_[cursor_] == 'e' || source_[cursor_] == 'E')) {
        ++cursor_;
        if (cursor_ < source_.size() && (source_[cursor_] == '+' || source_[cursor_] == '-'))
          ++cursor_;
        const auto exponentStart = cursor_;
        while (cursor_ < source_.size() && digit(source_[cursor_]))
          ++cursor_;
        if (cursor_ == exponentStart)
          syntax(start, "科学记数法缺少指数数字");
      }
      token_.kind = TokenKind::Number;
      token_.text = source_.substr(start, cursor_ - start);
      const auto result =
          std::from_chars(token_.text.data(), token_.text.data() + token_.text.size(),
                          token_.number, std::chars_format::general);
      if (result.ec != std::errc{} || result.ptr != token_.text.data() + token_.text.size() ||
          !std::isfinite(token_.number))
        syntax(start, "数值字面量格式错误、溢出或下溢");
      return;
    }
    if (letter(c)) {
      while (cursor_ < source_.size() && (letter(source_[cursor_]) || digit(source_[cursor_])))
        ++cursor_;
      token_.kind = TokenKind::Identifier;
      token_.text = source_.substr(start, cursor_ - start);
      return;
    }
    token_.text = std::string(1, c);
    if (c == '(')
      token_.kind = TokenKind::Left;
    else if (c == ')')
      token_.kind = TokenKind::Right;
    else if (c == ',')
      token_.kind = TokenKind::Comma;
    else if (c == '?')
      token_.kind = TokenKind::Question;
    else if (c == ':')
      token_.kind = TokenKind::Colon;
    else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '<' || c == '>' ||
             c == '!' || c == '=' || c == '&' || c == '|') {
      token_.kind = TokenKind::Operator;
      if (cursor_ < source_.size() &&
          (((c == '<' || c == '>' || c == '!' || c == '=') && source_[cursor_] == '=') ||
           ((c == '&' || c == '|') && source_[cursor_] == c)))
        token_.text += source_[cursor_++];
      if (token_.text == "=" || token_.text == "&" || token_.text == "|")
        syntax(start, "不支持赋值或按位运算符");
    } else {
      syntax(start, "不支持的字符");
    }
  }

  int add(Node node) {
    checkCancelled(context_);
    if (nodes_.size() == kMaxNodes)
      syntax(node.position, "AST 节点不能超过 1024 个");
    for (std::size_t i = 0; i < node.count; ++i)
      node.depth =
          std::max(node.depth, nodes_[static_cast<std::size_t>(node.children[i])].depth + 1);
    if (node.depth > kMaxDepth)
      syntax(node.position, "表达式 AST 深度不能超过 64 层");
    nodes_.push_back(std::move(node));
    return static_cast<int>(nodes_.size() - 1);
  }

  int operation(Kind kind, const Token& token, std::initializer_list<int> children) {
    Node node;
    node.kind = kind;
    node.position = token.position;
    node.text = token.text;
    node.count = children.size();
    std::copy(children.begin(), children.end(), node.children.begin());
    return add(std::move(node));
  }

  void nesting(std::size_t depth) const {
    if (depth > kMaxDepth)
      syntax(token_.position, "解析嵌套不能超过 64 层");
  }

  bool is(const char* op) const { return token_.kind == TokenKind::Operator && token_.text == op; }

  int conditional(std::size_t depth) {
    nesting(depth);
    int value = logicalOr(depth);
    if (token_.kind == TokenKind::Question) {
      const auto token = token_;
      next();
      const int yes = conditional(depth + 1);
      if (token_.kind != TokenKind::Colon)
        syntax(token_.position, "条件表达式缺少冒号");
      next();
      const int no = conditional(depth + 1);
      value = operation(Kind::Conditional, token, {value, yes, no});
    }
    return value;
  }

  int logicalOr(std::size_t depth) {
    int value = logicalAnd(depth);
    while (is("||")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, logicalAnd(depth)});
    }
    return value;
  }

  int logicalAnd(std::size_t depth) {
    int value = equality(depth);
    while (is("&&")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, equality(depth)});
    }
    return value;
  }

  int equality(std::size_t depth) {
    int value = comparison(depth);
    while (is("==") || is("!=")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, comparison(depth)});
    }
    return value;
  }

  int comparison(std::size_t depth) {
    int value = sum(depth);
    while (is("<") || is("<=") || is(">") || is(">=")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, sum(depth)});
    }
    return value;
  }

  int sum(std::size_t depth) {
    int value = product(depth);
    while (is("+") || is("-")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, product(depth)});
    }
    return value;
  }

  int product(std::size_t depth) {
    int value = unary(depth);
    while (is("*") || is("/")) {
      const auto token = token_;
      next();
      value = operation(Kind::Binary, token, {value, unary(depth)});
    }
    return value;
  }

  int unary(std::size_t depth) {
    nesting(depth);
    if (is("+") || is("-") || is("!")) {
      const auto token = token_;
      next();
      return operation(Kind::Unary, token, {unary(depth + 1)});
    }
    return power(depth);
  }

  int power(std::size_t depth) {
    const int value = primary(depth);
    if (!is("^"))
      return value;
    const auto token = token_;
    next();
    return operation(Kind::Binary, token, {value, unary(depth + 1)});
  }

  int primary(std::size_t depth) {
    nesting(depth);
    const auto token = token_;
    if (token.kind == TokenKind::Number) {
      next();
      Node node;
      node.position = token.position;
      node.number = token.number;
      return add(std::move(node));
    }
    if (token.kind == TokenKind::Left) {
      next();
      const int value = conditional(depth + 1);
      if (token_.kind != TokenKind::Right)
        syntax(token_.position, "缺少右括号");
      next();
      return value;
    }
    if (token.kind != TokenKind::Identifier)
      syntax(token.position, "需要数字、变量、函数或括号表达式");
    next();
    if (token.text == "true" || token.text == "false") {
      Node node;
      node.kind = Kind::Boolean;
      node.position = token.position;
      node.boolean = token.text == "true";
      return add(std::move(node));
    }
    if (token.text.size() == 1 && token.text[0] >= 'a' && token.text[0] <= 'h') {
      Node node;
      node.kind = Kind::Variable;
      node.position = token.position;
      node.text = token.text;
      return add(std::move(node));
    }
    const std::size_t arity =
        token.text == "abs" || token.text == "sqrt" || token.text == "floor" ||
                token.text == "ceil" || token.text == "round"
            ? 1
        : token.text == "min" || token.text == "max" || token.text == "pow" ? 2
        : token.text == "clamp"                                             ? 3
                                                                            : 0;
    if (!arity)
      syntax(token.position, "未知标识符或函数：" + token.text);
    if (token_.kind != TokenKind::Left)
      syntax(token_.position, "函数名后需要左括号");
    next();
    Node node;
    node.kind = Kind::Function;
    node.position = token.position;
    node.text = token.text;
    if (token_.kind != TokenKind::Right) {
      for (;;) {
        if (node.count == arity)
          syntax(token_.position, "函数参数数量过多");
        node.children[node.count++] = conditional(depth + 1);
        if (token_.kind != TokenKind::Comma)
          break;
        next();
      }
    }
    if (node.count != arity)
      syntax(token.position, "函数参数数量错误");
    if (token_.kind != TokenKind::Right)
      syntax(token_.position, "函数缺少右括号");
    next();
    return add(std::move(node));
  }

  const std::string& source_;
  ExecContext* context_;
  std::size_t cursor_ = 0;
  Token token_;
  std::vector<Node> nodes_;
};

struct Scalar {
  bool boolean = false;
  double number = 0;
  bool flag = false;
  std::string unit;
  std::string port;
};

class Evaluation {
public:
  Evaluation(const Program& program, const Inputs& inputs, ExecContext& context)
      : program_(program), inputs_(inputs), context_(context) {}

  Scalar run(int index) {
    checkCancelled(&context_);
    const auto& node = program_.nodes[static_cast<std::size_t>(index)];
    if (node.kind == Kind::Number)
      return {false, node.number, false, {}, {}};
    if (node.kind == Kind::Boolean)
      return {true, 0, node.boolean, {}, {}};
    if (node.kind == Kind::Variable)
      return variable(node);
    Scalar a = run(node.children[0]);
    if (node.kind == Kind::Unary) {
      if (node.text == "!") {
        requireBoolean(a, node);
        a.flag = !a.flag;
      } else {
        requireNumber(a, node);
        if (node.text == "-")
          a.number = -a.number;
      }
      return a;
    }
    if (node.kind == Kind::Conditional) {
      requireBoolean(a, node);
      return run(node.children[a.flag ? 1 : 2]);
    }
    if (node.kind == Kind::Binary && (node.text == "&&" || node.text == "||")) {
      requireBoolean(a, node);
      if ((node.text == "&&" && !a.flag) || (node.text == "||" && a.flag))
        return a;
      Scalar b = run(node.children[1]);
      requireBoolean(b, node);
      return b;
    }
    if (node.kind == Kind::Function)
      return function(node, std::move(a));
    Scalar b = run(node.children[1]);
    const auto& op = node.text;
    if (op == "==" || op == "!=") {
      if (a.boolean != b.boolean)
        invalid(node.position, "相等比较要求相同值类型", origin(a, b));
      bool equal = false;
      if (a.boolean)
        equal = a.flag == b.flag;
      else {
        sameUnit(a, b, node);
        equal = a.number == b.number;
      }
      return {true, 0, op == "==" ? equal : !equal, {}, origin(a, b)};
    }
    requireNumber(a, node);
    requireNumber(b, node);
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
      sameUnit(a, b, node);
      const bool result = op == "<"    ? a.number < b.number
                          : op == "<=" ? a.number <= b.number
                          : op == ">"  ? a.number > b.number
                                       : a.number >= b.number;
      return {true, 0, result, {}, origin(a, b)};
    }
    Scalar result;
    result.port = origin(a, b);
    if (op == "+" || op == "-") {
      sameUnit(a, b, node);
      result.unit = a.unit;
      result.number = op == "+" ? a.number + b.number : a.number - b.number;
    } else if (op == "*") {
      if (!a.unit.empty() && !b.unit.empty())
        invalid(node.position, "乘法至少一端必须无单位", result.port);
      result.unit = a.unit.empty() ? b.unit : a.unit;
      result.number = a.number * b.number;
    } else if (op == "/") {
      if (b.number == 0)
        invalid(node.position, "除数不能为零", b.port);
      if (!b.unit.empty() && a.unit != b.unit)
        invalid(node.position, "除法要求无单位除数或相同单位相消", result.port);
      result.unit = b.unit.empty() ? a.unit : "";
      result.number = a.number / b.number;
    } else {
      result = power(a, b, node);
    }
    finite(result, node);
    return result;
  }

private:
  static std::string origin(const Scalar& a, const Scalar& b) {
    return a.port.empty() ? b.port : a.port;
  }

  static void requireNumber(const Scalar& value, const Node& node) {
    if (value.boolean)
      invalid(node.position, "需要 number 值，不进行隐式布尔转换", value.port);
  }

  static void requireBoolean(const Scalar& value, const Node& node) {
    if (!value.boolean)
      invalid(node.position, "需要 boolean 值，不进行数值真值转换", value.port);
  }

  static void sameUnit(const Scalar& a, const Scalar& b, const Node& node) {
    if (a.unit != b.unit)
      invalid(node.position, "数值单位必须精确一致，不进行隐式单位换算", origin(a, b));
  }

  static void finite(const Scalar& result, const Node& node) {
    if (!std::isfinite(result.number))
      invalid(node.position, "计算结果溢出或不在有限实数域", result.port);
  }

  static Scalar power(const Scalar& a, const Scalar& b, const Node& node) {
    if (!a.unit.empty() || !b.unit.empty())
      invalid(node.position, "幂运算仅支持无单位输入", origin(a, b));
    if (a.number == 0 && b.number <= 0)
      invalid(node.position, "零的零次幂或负次幂没有有效定义", origin(a, b));
    if (a.number < 0 && std::trunc(b.number) != b.number)
      invalid(node.position, "负底数的非整数幂不在实数域", origin(a, b));
    Scalar result;
    result.port = origin(a, b);
    result.number = std::pow(a.number, b.number);
    finite(result, node);
    return result;
  }

  Scalar variable(const Node& node) {
    if (!inputs_.has(node.text))
      invalid(node.position, "所求分支需要连接输入 " + node.text, node.text);
    const Record* record = nullptr;
    std::string why;
    if (!values::readValue(inputs_.get(node.text), record, why))
      invalid(node.position, why, node.text);
    const auto kind = record->data["kind"].get<std::string>();
    if (kind != "number" && kind != "boolean")
      invalid(node.position, "表达式仅支持 number 或 boolean 输入", node.text);
    Scalar result;
    result.boolean = kind == "boolean";
    result.port = node.text;
    result.unit = record->data["unit"].get<std::string>();
    if (result.boolean)
      result.flag = record->data["value"].get<bool>();
    else
      result.number = record->data["value"].get<double>();
    return result;
  }

  Scalar function(const Node& node, Scalar a) {
    requireNumber(a, node);
    const auto& name = node.text;
    if (name == "abs")
      a.number = std::abs(a.number);
    else if (name == "floor")
      a.number = std::floor(a.number);
    else if (name == "ceil")
      a.number = std::ceil(a.number);
    else if (name == "round")
      a.number = std::round(a.number);
    else if (name == "sqrt") {
      if (!a.unit.empty())
        invalid(node.position, "sqrt 仅支持无单位输入", a.port);
      if (a.number < 0)
        invalid(node.position, "sqrt 的输入不能为负数", a.port);
      a.number = std::sqrt(a.number);
    } else {
      const Scalar b = run(node.children[1]);
      requireNumber(b, node);
      if (name == "pow")
        return power(a, b, node);
      sameUnit(a, b, node);
      if (name == "min" || name == "max") {
        a.number = name == "min" ? std::min(a.number, b.number) : std::max(a.number, b.number);
        a.port = origin(a, b);
      } else {
        const Scalar c = run(node.children[2]);
        requireNumber(c, node);
        sameUnit(a, c, node);
        if (b.number > c.number)
          invalid(node.position, "clamp 的下界不能大于上界", origin(b, c));
        a.number = std::clamp(a.number, b.number, c.number);
        a.port = origin(a, b);
        if (a.port.empty())
          a.port = c.port;
      }
    }
    finite(a, node);
    return a;
  }

  const Program& program_;
  const Inputs& inputs_;
  ExecContext& context_;
};

Status computeExpression(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                         ExecContext& context) {
  try {
    const auto program = Parser(params.text("expression"), &context).parse();
    const auto result = Evaluation(program, inputs, context).run(program.root);
    checkCancelled(&context);
    Record record =
        result.boolean ? values::boolean(result.flag) : values::number(result.number, result.unit);
    std::string why;
    if (!values::validate(record, why))
      return Status::Error(Phase::Execute, "bad_input", why, {}, result.port);
    outputs.set("value", Data::record(std::move(record)));
    return Status::Ok();
  } catch (const Failure& failure) {
    return failure.status;
  }
}

std::vector<Issue> validateExpression(const ParamView& params, const std::set<std::string>&) {
  try {
    Parser(params.text("expression"), nullptr).parse();
    return {};
  } catch (const Failure& failure) {
    return {Issue::error(failure.status.code, failure.status.message, "expression")};
  }
}

} // namespace

void registerExpression(Registry& registry) {
  OperatorDesc descriptor;
  descriptor.id = "value.expression";
  descriptor.version = "1.0.0";
  descriptor.label = "受限表达式计算";
  descriptor.category = "通用/值与集合";
  descriptor.keywords = {"expression", "formula", "arithmetic", "condition", "math",
                         "表达式",     "公式",    "计算",       "条件判定"};
  descriptor.doc =
      "有限数值与布尔表达式；a..h 为可选变量，输入单位严格校验。支持四则、右结合幂、比较、逻辑及条件短路。函数限 abs/min/max/clamp/sqrt/pow/floor/ceil/round。最多4096字节、1024节点、64层。连接上游仍正常调度。";
  const auto contract = nlohmann::json{{"recordType", values::kValueType}};
  for (char name = 'a'; name <= 'h'; ++name) {
    const std::string port(1, name);
    const std::string label(1, static_cast<char>(name - 'a' + 'A'));
    descriptor.inputs.push_back(
        withContract(Port{port, "Record", label, "std.Value.v1", false}, contract));
  }
  descriptor.outputs = {
      withContract(Port{"value", "Record", "结果", "std.Value.v1", true}, contract)};
  Param source;
  source.name = "expression";
  source.label = "表达式";
  source.type = ParamType::Text;
  source.def = Value::text("(a+b)/2");
  source.rows = 4;
  source.doc =
      "变量a..h；&&、||和?:仅在表达式内部短路。单位不隐式换算；语法错误位置按1起始UTF-8字节计。";
  descriptor.params = {std::move(source)};
  descriptor.capabilities = {false, true, true};
  descriptor.compute = &computeExpression;
  descriptor.validate = &validateExpression;
  registry.addOperator(std::move(descriptor));
}

} // namespace lyflow::packs::std_value
