#pragma once
//
// LyFlow 算子描述数据结构。
//
// 这是三层契约的 C++ 侧真实来源。序列化结果必须符合
// schema/operator-manifest.schema.json —— 改这里等于改跨语言 API。
//
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lyflow {

// ---------------------------------------------------------------------------
// Value —— 参数默认值。只覆盖 schema 中 param.default 允许出现的形态。
// ---------------------------------------------------------------------------
class Value {
 public:
  enum class Kind { Null, Bool, Int, Float, String, FloatVec };

  Value() = default;

  static Value boolean(bool v);
  static Value integer(std::int64_t v);
  static Value number(double v);
  static Value text(std::string v);
  static Value vec(std::vector<double> v);

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::Null; }

  bool boolValue() const { return bool_; }
  std::int64_t intValue() const { return int_; }
  double floatValue() const { return float_; }
  const std::string& stringValue() const { return string_; }
  const std::vector<double>& vecValue() const { return vec_; }

 private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  std::int64_t int_ = 0;
  double float_ = 0.0;
  std::string string_;
  std::vector<double> vec_;
};

// ---------------------------------------------------------------------------
// 端口类型表
// ---------------------------------------------------------------------------
struct PortType {
  std::string name;
  std::string color;                      // 端口与连线着色，前端直接用
  std::vector<std::string> castableTo;    // 可隐式转换到的类型
  std::string doc;
};

struct Port {
  std::string name;                       // 用名字不用序号，增删端口老图不错位
  std::string type;                       // 必须存在于类型表中，startup 时自检
  std::string label;
  std::string doc;
  bool required = true;                   // 仅对 inputs 有意义
};

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------
enum class ParamType {
  Bool, Int, Float, Vec2f, Vec3f, Vec4f, Enum, Flags,
  String, Text, Path, Color, Transform, Curve,
};

// 与 schema 的 param.type enum 一一对应。新增类型要同步改 schema 和前端控件表。
const char* toString(ParamType t);

struct EnumOption {
  std::string value;
  std::string label;
  std::string doc;
};

// 参数联动条件。有意做得很弱：只支持对同节点其他参数的等值/包含判断。
// 需要更复杂的逻辑通常说明这个算子该拆了。
struct Condition {
  std::string param;             // 空 = 未设置条件
  Value eq;                      // Kind::Null = 未设置
  std::vector<Value> in;

  bool isSet() const { return !param.empty(); }
};

struct FileFilter {
  std::string name;
  std::vector<std::string> extensions;
};

struct Param {
  std::string name;
  ParamType type = ParamType::Float;
  std::string label;
  std::string doc;
  Value def;                              // 必填。GraphDoc 稀疏存储以此为基准
  std::string group;
  bool advanced = false;

  std::optional<double> min;              // 硬边界，超出即非法
  std::optional<double> max;
  std::optional<double> softMin;          // 滑块范围，可手输超出
  std::optional<double> softMax;
  std::optional<double> step;
  std::string unit;

  std::vector<std::string> componentLabels;  // vecNf 分量名，默认 X/Y/Z/W
  std::vector<EnumOption> options;           // enum / flags
  std::string placeholder;
  std::string pattern;
  std::optional<int> rows;                   // text
  std::vector<FileFilter> filters;           // path
  std::string mode;                          // path: open | save | dir
  std::optional<bool> alpha;                 // color

  Condition visibleWhen;
  Condition enabledWhen;
};

struct Capabilities {
  bool cancellable = false;     // 支持中途取消 -> 可用于 live preview
  bool previewable = false;     // 支持降级质量的快速预览
  bool deterministic = true;    // 同输入同输出 -> 可参与缓存
};

struct OperatorDesc {
  std::string id;                         // 全局唯一，用 . 分命名空间
  std::string version;                    // semver
  std::vector<std::string> aliases;       // 旧 id，重命名后自动重定向
  std::string label;
  std::string category;                   // 用 / 分层，决定搜索面板树形结构
  std::vector<std::string> keywords;
  std::string doc;

  std::vector<Port> inputs;
  std::vector<Port> outputs;
  std::vector<Param> params;
  Capabilities capabilities;
};

}  // namespace lyflow
