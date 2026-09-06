#pragma once
// 算子描述数据结构 —— 三层契约的 C++ 侧真实来源。
// 序列化结果必须符合 schema/operator-manifest.schema.json，改这里等于改跨语言 API。
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/status.h"

namespace lyflow {

// 算子的计算实体，定义在 operator.h。这里用不完整类型声明函数指针，
// 免得 manifest.h 反过来依赖 operator.h（后者要用本文件的 Value）。
class Inputs;
class ParamView;
class Outputs;
class ExecContext;

/// 算子的计算函数。输入已按端口类型校验过，参数已合并默认值，ctx 提供取消与进度。
using ComputeFn = Status (*)(const Inputs&, const ParamView&, Outputs&, ExecContext&);

/// 可选钩子：把「文件内容变了」这类外部状态揉进 cacheKey。
/// IO 算子返回 "size:mtime"，读不到文件时返回空串（当作没有外部状态）。
using ExternalKeyFn = std::string (*)(const ParamView&);

/// 主版本升级时的参数改写：拿旧参数对象，还一份新的（ADR-0008）。
/// 只碰参数 —— 端口改名要靠新算子 + aliases，因为连线不归算子管。
using MigrateFn = nlohmann::json (*)(const nlohmann::json&);

/// 从 fromMajor 升到 fromMajor+1 的一步。链条必须覆盖 1..current-1，
/// 缺一环 Registry::validate() 就报 —— 半条链比没有链更难查。
struct Migration {
  int fromMajor = 1;
  MigrateFn apply = nullptr;
};

// ------------------------------- Value —— 参数默认值（只覆盖 param.default 的形态）
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

// ---------------------------------------------------------------------- 端口类型表
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
  // 下面两条只对 inputs 有意义，语义见 ADR-0016。新字段一律追加在末尾：
  // 现有算子用聚合初始化写了前五个成员，插在中间会静默改掉它们的 required。
  bool acceptsError = false;
  bool lazy = false;
  /// 同一节点上、同一组的 Any 端口共用一个类型变量。默认全在 0 组（历史语义）。
  /// flow.select 的 cond 与 a/b/out 不是一回事，靠它分开。不进 manifest。
  int anyGroup = 0;
};

// --------------------------------------------------------------------------- 参数
enum class ParamType {
  Bool, Int, Float, Vec2f, Vec3f, Vec4f, Enum, Flags,
  String, Text, Path, Color, Transform, Curve,
};

// 与 schema 的 param.type enum 一一对应。新增类型要同步改 schema 和前端控件表。
const char* toString(ParamType t);

/// toString 的逆。认不出来返回 false，out 不动（子图/库算子的参数声明是数据，不是代码）。
bool parseParamType(const std::string& name, ParamType& out);

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
  std::string pack;                       // 来自哪个算子包，「名字」或「名字@版本」；core 自带的留空
  std::vector<std::string> aliases;       // 旧 id，重命名后自动重定向
  std::string label;
  std::string category;                   // 用 / 分层，决定搜索面板树形结构
  std::vector<std::string> keywords;
  std::string doc;

  std::vector<Port> inputs;
  std::vector<Port> outputs;
  std::vector<Param> params;
  Capabilities capabilities;

  /// 计算实体。Registry::validate() 要求非空 —— 一个「有描述没实现」的算子
  /// 会在用户点运行时才暴露，那时已经画完整张图了。
  ComputeFn compute = nullptr;
  ExternalKeyFn externalKey = nullptr;

  /// 参数迁移链，按 fromMajor 任意顺序给都行。空 = 该算子从没破坏性升级过。
  std::vector<Migration> migrations;
};

/// 按名字找参数描述。执行器和算子都要用，放这里免得各写一遍线性查找。
const Param* findParam(const OperatorDesc& op, const std::string& name);

/// 「一段文本 → 一张图」。text 是原文，baseDir 是它所在目录（相对路径参数据此写）。
/// 成功时把 GraphDoc JSON 写进 graphJson；失败时返回 Status（ADR-0017）。
using ImportFn = Status (*)(const std::string& text, const std::filesystem::path& baseDir,
                            std::string& graphJson);

/// 一种可导入的外部格式。kind 是调用方给 lyflow_import 的第一个参数。
struct ImporterDesc {
  std::string kind;
  std::string label;
  std::string doc;
  std::string pack;
  ImportFn fn = nullptr;
};

}  // namespace lyflow
