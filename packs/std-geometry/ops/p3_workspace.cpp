#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

#include "lyflow/kernel/rigid.h"
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::ops {
namespace {
namespace pose_detail = lyflow::kernel;
using namespace pose_detail;
inline Status bad(const std::string& text, const char* port = "poses") {
  return Status::Error(Phase::Execute, "bad_input", text, {}, port);
}
inline Status badParam(const std::string& text, const char* name) {
  return Status::Error(Phase::Execute, "bad_param", text, name);
}
inline Param text(const char* name, const char* label, const char* def = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::String;
  p.def = Value::text(def);
  return p;
}
inline Param number(const char* name, const char* label, double def = 0, const char* unit = "") {
  Param p;
  p.name = name;
  p.label = label;
  p.type = ParamType::Float;
  p.def = Value::number(def);
  p.unit = unit;
  return p;
}
inline Param integer(const char* name, const char* label, std::int64_t def = 0) {
  Param p = number(name, label);
  p.type = ParamType::Int;
  p.def = Value::integer(def);
  return p;
}
inline Param flag(const char* name, const char* label, bool def = false) {
  Param p = text(name, label);
  p.type = ParamType::Bool;
  p.def = Value::boolean(def);
  return p;
}
inline Param vec(const char* name, const char* label, std::vector<double> values) {
  Param p = text(name, label);
  p.type = values.size() == 2   ? ParamType::Vec2f
           : values.size() == 3 ? ParamType::Vec3f
                                : ParamType::Vec4f;
  p.def = Value::vec(std::move(values));
  return p;
}
inline Param choice(const char* name, const char* label, const char* def,
                    std::initializer_list<const char*> values) {
  Param p = text(name, label, def);
  p.type = ParamType::Enum;
  for (const char* value : values)
    p.options.push_back({value, value, ""});
  return p;
}
inline OperatorDesc descriptor(const char* id, const char* label, const char* keywords) {
  OperatorDesc op;
  op.id = id;
  op.version = "1.0.0";
  op.label = label;
  op.category = "几何/工作空间";
  op.keywords = {keywords, label, "pose", "工件", "位姿", "坐标系"};
  op.capabilities = {true, false, true};
  return op;
}
inline Port record(const char* name, bool required = true) {
  return {name, "Record", name, "版本化 Record，具体载荷与单位见算子说明。", required};
}
inline Port scalarPort(const char* name, bool required = false) {
  return withContract(
      Port{name, "Record", name, "std.Value.v1；数值单位必须匹配算子声明。", required},
      {{"recordType", values::kValueType}});
}

constexpr std::size_t kMaxFile = 1024 * 1024;
constexpr std::size_t kMaxFrames = 256;
constexpr std::size_t kMaxNodes = 32768;
constexpr double kCoordinateLimit = 1e12;

struct Failure {
  Status status;
};

void require(bool condition, const std::string& message, const char* code = "bad_input") {
  if (!condition)
    throw Failure{Status::Error(Phase::Execute, code, message, "path")};
}
void poll(ExecContext& ctx) { require(!ctx.cancelled(), "工作空间加载已取消", "cancelled"); }
M4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
M4 compose(const M4& a, const M4& b) {
  M4 result{};
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      result[4 * row + column] = productSum({{a[4 * row], b[column]},
                                             {a[4 * row + 1], b[4 + column]},
                                             {a[4 * row + 2], b[8 + column]},
                                             {a[4 * row + 3], b[12 + column]}});
  return result;
}
M4 inverse(const M4& m) {
  auto result = identity();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      result[4 * row + column] = m[4 * column + row];
    result[4 * row + 3] = -productSum({{m[row], m[3]}, {m[4 + row], m[7]}, {m[8 + row], m[11]}});
  }
  return result;
}
void name(const std::string& value) {
  require(!value.empty() && value.size() <= 128 &&
              value.find_first_not_of(' ') != std::string::npos,
          "工作空间/参考帧标识必须为1..128字节非空名称");
  for (unsigned char c : value)
    require(c >= 32 && c != 127, "工作空间/参考帧标识不得含控制字符");
}
std::string label(const Json& value) {
  require(value.is_string(), "工作空间/参考帧标识必须是字符串");
  auto result = value.get<std::string>();
  name(result);
  return result;
}
void fields(const Json& object, std::initializer_list<const char*> expected) {
  require(object.is_object() && object.size() == expected.size(), "工作空间字段不完整或含未知字段");
  for (const auto* key : expected)
    require(object.contains(key), std::string("缺少工作空间字段：") + key);
}
V3 vector3(const Json& value, bool positive = false) {
  require(poses::numericArray(value, 3, positive), "尺寸/坐标必须是有限三分量数组且尺寸为正");
  V3 result = value.get<V3>();
  for (double v : result)
    require(std::abs(v) <= kCoordinateLimit, "工作空间尺寸/坐标超过1e12米预算");
  return result;
}
void rigidMatrix(const M4& m) {
  for (double value : m)
    require(std::isfinite(value), "工作空间变换必须全部有限");
  require(m[12] == 0 && m[13] == 0 && m[14] == 0 && m[15] == 1, "工作空间变换末行必须为[0,0,0,1]");
  for (int axis = 0; axis < 3; ++axis)
    require(std::abs(m[4 * axis + 3]) <= kCoordinateLimit, "工作空间平移超过1e12米预算");
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) {
      const double value = m[a] * m[b] + m[4 + a] * m[4 + b] + m[8 + a] * m[8 + b];
      require(std::abs(value - (a == b ? 1 : 0)) <= 1e-8, "工作空间旋转必须正交，不允许尺度或剪切");
    }
  require(dot(cross({m[0], m[4], m[8]}, {m[1], m[5], m[9]}), {m[2], m[6], m[10]}) > 0,
          "工作空间旋转必须为右手，不允许镜像");
}
M4 matrix(const Json& value) {
  require(poses::numericArray(value, 16), "工作空间transform必须是行优先16项有限数组");
  const auto result = value.get<M4>();
  rigidMatrix(result);
  return result;
}

struct Frame {
  std::string parent;
  M4 local = identity();
  M4 root = identity();
  int state = 0;
};
struct Workspace {
  std::string id, frame, anchor;
  V3 size;
};
struct Tree {
  std::string root;
  std::map<std::string, Frame> frames;
  std::vector<Workspace> workspaces;
};
void resolve(Tree& tree, const std::string& id, ExecContext& ctx) {
  poll(ctx);
  auto& frame = tree.frames.at(id);
  require(frame.state != 1, "工作空间参考帧树存在环");
  if (frame.state == 2)
    return;
  frame.state = 1;
  if (id == tree.root) {
    frame.root = identity();
  } else {
    require(tree.frames.count(frame.parent) != 0, "工作空间参考帧的父级不存在");
    resolve(tree, frame.parent, ctx);
    frame.root = compose(tree.frames.at(frame.parent).root, frame.local);
    rigidMatrix(frame.root);
  }
  frame.state = 2;
}
void resolve(Tree& tree, ExecContext& ctx) {
  for (auto& entry : tree.frames)
    entry.second.state = 0;
  for (const auto& entry : tree.frames)
    resolve(tree, entry.first, ctx);
}
Tree nativeTree(const Json& document) {
  fields(document, {"schemaVersion", "unit", "rootFrame", "frames", "workspaces"});
  require(document["schemaVersion"].is_number_integer() && document["schemaVersion"] == 1,
          "仅支持工作空间文件schemaVersion=1");
  require(document["unit"] == "m", "工作空间文件必须显式声明unit=m");
  Tree tree;
  tree.root = label(document["rootFrame"]);
  tree.frames.emplace(tree.root, Frame{});
  const auto& frames = document["frames"];
  const auto& workspaces = document["workspaces"];
  require(frames.is_array() && workspaces.is_array() && !workspaces.empty(),
          "frames/workspaces必须为数组且工作空间不得为空");
  require(frames.size() < kMaxFrames && workspaces.size() <= kMaxFrames,
          "参考帧总数/工作空间数最多256", "resource_limit");
  for (const auto& item : frames) {
    fields(item, {"id", "parent", "transform"});
    const auto id = label(item["id"]);
    Frame frame;
    frame.parent = label(item["parent"]);
    frame.local = matrix(item["transform"]);
    require(tree.frames.emplace(id, std::move(frame)).second, "参考帧id重复或覆盖根帧");
  }
  std::set<std::string> ids;
  for (const auto& item : workspaces) {
    fields(item, {"id", "frame", "size", "anchor"});
    Workspace workspace{label(item["id"]), label(item["frame"]), label(item["anchor"]),
                        vector3(item["size"], true)};
    require(workspace.anchor == "bottom" || workspace.anchor == "center",
            "工作空间anchor仅支持bottom/center");
    require(ids.insert(workspace.id).second && tree.frames.count(workspace.frame) != 0,
            "工作空间id重复或引用未知参考帧");
    tree.workspaces.push_back(std::move(workspace));
  }
  return tree;
}

class YamlBudget final : public YAML::EventHandler {
public:
  explicit YamlBudget(ExecContext& ctx) : ctx_(ctx) {}
  void OnDocumentStart(const YAML::Mark&) override {}
  void OnDocumentEnd() override {}
  void OnNull(const YAML::Mark&, YAML::anchor_t anchor) override { scalar(anchor); }
  void OnAlias(const YAML::Mark&, YAML::anchor_t) override {
    require(false, "工作空间YAML不接受alias/anchor");
  }
  void OnScalar(const YAML::Mark&, const std::string&, YAML::anchor_t anchor,
                const std::string& value) override {
    scalar(anchor);
    require(value.size() <= values::kMaxStringBytes, "YAML标量超过64KiB预算", "resource_limit");
  }
  void OnSequenceStart(const YAML::Mark&, const std::string&, YAML::anchor_t anchor,
                       YAML::EmitterStyle::value) override {
    start(anchor);
  }
  void OnSequenceEnd() override { --depth_; }
  void OnMapStart(const YAML::Mark&, const std::string&, YAML::anchor_t anchor,
                  YAML::EmitterStyle::value) override {
    start(anchor);
  }
  void OnMapEnd() override { --depth_; }

private:
  ExecContext& ctx_;
  std::size_t nodes_ = 0, depth_ = 0;
  void scalar(YAML::anchor_t anchor) {
    require(anchor == 0, "工作空间YAML不接受alias/anchor");
    require(++nodes_ <= kMaxNodes, "工作空间YAML节点超过预算", "resource_limit");
    if ((nodes_ & 255) == 0)
      poll(ctx_);
  }
  void start(YAML::anchor_t anchor) {
    scalar(anchor);
    require(++depth_ <= 16, "工作空间YAML嵌套超过16层", "resource_limit");
  }
};
M4 yamlMatrix(const YAML::Node& node) {
  require(node.IsSequence() && node.size() == 4, "外部YAML pose必须是嵌套4×4行矩阵");
  M4 result{};
  for (int row = 0; row < 4; ++row) {
    require(node[row].IsSequence() && node[row].size() == 4, "外部YAML pose必须是嵌套4×4行矩阵");
    for (int col = 0; col < 4; ++col) {
      require(node[row][col].IsScalar(), "外部YAML矩阵项必须为有限数");
      result[4 * row + col] = node[row][col].as<double>();
    }
  }
  rigidMatrix(result);
  return result;
}
Tree legacyTree(const std::string& bytes, const ParamView& p, ExecContext& ctx) {
  require(p.choice("legacyUnit") == "m", "外部无版本YAML必须显式选择legacyUnit=m", "bad_param");
  std::istringstream stream(bytes);
  YAML::Parser parser(stream);
  YamlBudget budget(ctx);
  require(parser.HandleNextDocument(budget) && !parser.HandleNextDocument(budget),
          "工作空间YAML必须恰有一个文档");
  const auto document = YAML::Load(bytes);
  require(document.IsMap(), "外部工作空间YAML必须是根映射");
  const std::set<std::string> allowed{
      "pose", "length", "width", "height", "parent_id", "parent_frame_type", "parent_pose"};
  std::set<std::string> keys;
  for (auto entry : document) {
    require(entry.first.IsScalar(), "外部YAML字段名必须是标量");
    const auto key = entry.first.as<std::string>();
    require(allowed.count(key) != 0 && keys.insert(key).second,
            "外部YAML有未知/重复字段；相机标定及未知资产格式不受支持");
  }
  for (const auto* key : {"pose", "length", "width", "height"})
    require(keys.count(key) != 0, std::string("外部工作空间缺少字段：") + key);
  const auto parentKeys =
      keys.count("parent_id") + keys.count("parent_frame_type") + keys.count("parent_pose");
  require(parentKeys == 0 || parentKeys == 3, "外部parent_id/type/pose必须同时提供");
  Tree tree;
  tree.root = p.text("legacyRootFrame");
  name(tree.root);
  tree.frames.emplace(tree.root, Frame{});
  Workspace workspace{p.text("workspaceId"), p.text("workspaceId"), "bottom", {}};
  name(workspace.id);
  for (int i = 0; i < 3; ++i) {
    const auto key = std::array<const char*, 3>{"length", "width", "height"}[i];
    require(document[key].IsScalar(), "外部工作空间尺寸必须为数值标量");
    workspace.size[i] = document[key].as<double>();
    require(std::isfinite(workspace.size[i]) && workspace.size[i] > 0 &&
                workspace.size[i] <= kCoordinateLimit,
            "外部工作空间尺寸必须为(0,1e12]米");
  }
  Frame frame;
  frame.parent = tree.root;
  frame.local = yamlMatrix(document["pose"]);
  if (parentKeys) {
    require(document["parent_id"].IsScalar() && document["parent_frame_type"].IsScalar(),
            "外部父帧类型和id必须为标量");
    const auto type = document["parent_frame_type"].as<std::string>();
    const std::set<std::string> types{"World", "Camera", "Workspace", "Hand",
                                      "Robot", "Joint",  "Custom"};
    require(types.count(type) != 0, "外部父帧类型不受支持");
    frame.parent = type + document["parent_id"].as<std::string>();
    name(frame.parent);
    const auto parent = yamlMatrix(document["parent_pose"]);
    if (frame.parent == tree.root)
      require(parent == identity(), "根帧parent_pose必须为恒等，不能静默覆盖根参考帧");
    else {
      Frame entry;
      entry.parent = tree.root;
      entry.local = parent;
      tree.frames.emplace(frame.parent, entry);
    }
  }
  require(tree.frames.emplace(workspace.frame, frame).second, "外部workspaceId与父帧/根帧冲突");
  tree.workspaces.push_back(std::move(workspace));
  return tree;
}

std::string read(const std::filesystem::path& path, ExecContext& ctx) {
  std::error_code error;
  require(std::filesystem::is_regular_file(path, error) && !error, "工作空间路径不是可读普通文件",
          "io");
  const auto size = std::filesystem::file_size(path, error);
  require(!error && size <= kMaxFile, "工作空间文件超过1MiB预算", "resource_limit");
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "无法读取工作空间文件", "io");
  std::string result;
  std::array<char, 65536> buffer{};
  while (stream) {
    poll(ctx);
    stream.read(buffer.data(), buffer.size());
    const auto count = static_cast<std::size_t>(stream.gcount());
    require(count <= kMaxFile - result.size(), "读取期间工作空间文件超过1MiB预算",
            "resource_limit");
    result.append(buffer.data(), count);
  }
  require(stream.eof(), "工作空间文件读取失败", "io");
  require(!result.empty(), "工作空间文件为空");
  return result;
}
std::string fingerprint(const std::string& bytes) {
  std::uint64_t value = 14695981039346656037ULL;
  for (unsigned char byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  std::ostringstream stream;
  stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << value;
  return stream.str();
}
Json jsonDocument(const std::string& bytes, ExecContext& ctx) {
  std::size_t nodes = 0;
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    require(depth <= 16 && ++nodes <= kMaxNodes, "工作空间JSON深度/节点数超过预算",
            "resource_limit");
    if ((nodes & 255) == 0)
      poll(ctx);
    if (event == Json::parse_event_t::object_start)
      keys.emplace_back();
    else if (event == Json::parse_event_t::key)
      require(keys.back().insert(value.get<std::string>()).second, "工作空间JSON有重复字段");
    else if (event == Json::parse_event_t::object_end)
      keys.pop_back();
    return true;
  };
  auto result = Json::parse(bytes, callback);
  std::string why;
  require(values::boundedJson(result, why), "工作空间JSON无效：" + why);
  return result;
}
poses::Quaternion orientation(const M4& m) {
  return poses::quaternion({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]});
}
M4 poseMatrix(const Record& pose) {
  const auto r = poses::rotationMatrix(pose.data["rotation"].get<poses::Quaternion>());
  auto m = identity();
  const auto p = vector3(pose.data["position"]);
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      m[4 * row + column] = r[3 * row + column];
    m[4 * row + 3] = p[row];
  }
  return m;
}
Transform native(const M4& m, double rotationError) {
  Transform result;
  for (int i = 0; i < 16; ++i) {
    require(std::isfinite(m[i]) && std::abs(m[i]) <= std::numeric_limits<float>::max(),
            "工作空间变换超出原生float32范围");
    result.m[i] = static_cast<float>(m[i]);
    if (i % 4 != 3)
      require(std::abs(static_cast<double>(result.m[i]) - m[i]) <= rotationError,
              "工作空间旋转的float32量化超过maxRotationError");
  }
  M4 checked;
  poses::Quaternion q;
  std::string why;
  require(rigid(&result, checked, q, why), "工作空间原生变换不是右手刚体：" + why);
  return result;
}
V3 nativeApply(const Transform& m, V3 point) {
  const float x = static_cast<float>(point[0]), y = static_cast<float>(point[1]),
              z = static_cast<float>(point[2]);
  V3 result;
  for (int row = 0; row < 3; ++row) {
    const auto* a = m.m + 4 * row;
    result[row] = a[0] * x + a[1] * y + a[2] * z + a[3];
  }
  return result;
}
double distance(V3 a, V3 b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

Status load(const Inputs& in, const ParamView& p, Outputs& out, ExecContext& ctx) {
  try {
    poll(ctx);
    const auto selectedId = p.text("workspaceId");
    name(selectedId);
    const double translationError = p.number("maxTranslationError"),
                 rotationError = p.number("maxRotationError");
    require(std::isfinite(translationError) && translationError > 0 && translationError <= 1e6 &&
                std::isfinite(rotationError) && rotationError > 0 && rotationError <= 1e-5,
            "原生交付误差门限非法", "bad_param");
    const auto bytes = read(p.path("path"), ctx);
    Tree tree;
    const auto format = p.choice("format");
    if (format == "lyflow-json-v1")
      tree = nativeTree(jsonDocument(bytes, ctx));
    else if (format == "xyz-unversioned-yaml")
      tree = legacyTree(bytes, p, ctx);
    else
      require(false, "未知工作空间文件格式", "bad_param");
    resolve(tree, ctx);
    const auto target = p.text("targetFrame").empty() ? tree.root : p.text("targetFrame");
    name(target);
    require(tree.frames.count(target) != 0, "targetFrame不在工作空间参考帧树内", "bad_param");
    auto selected = std::find_if(tree.workspaces.begin(), tree.workspaces.end(),
                                 [&](const auto& ws) { return ws.id == selectedId; });
    require(selected != tree.workspaces.end(), "workspaceId不存在", "bad_param");
    std::string overrideId;
    if (in.has("overrideRegion")) {
      const auto* pose = in.get("overrideRegion").asRecord();
      std::string why;
      require(pose && pose->type == poses::kPose3DType && poses::validate(*pose, why) &&
                  pose->data["frame"] == target,
              "overrideRegion必须是targetFrame中的单个米制Pose3D：" + why);
      require(selected->frame != tree.root, "不能用overrideRegion移动根参考帧");
      for (auto cursor = target; !cursor.empty(); cursor = tree.frames.at(cursor).parent)
        require(cursor != selected->frame,
                "overrideRegion的目标参考帧不能是被移动的工作空间帧或其子帧");
      selected->size = vector3(pose->data["size"], true);
      auto targetToWorkspace = poseMatrix(*pose);
      if (selected->anchor == "bottom") {
        auto shift = identity();
        shift[11] = -selected->size[2] / 2;
        targetToWorkspace = compose(targetToWorkspace, shift);
      }
      auto& frame = tree.frames.at(selected->frame);
      frame.local = compose(inverse(tree.frames.at(frame.parent).root),
                            compose(tree.frames.at(target).root, targetToWorkspace));
      rigidMatrix(frame.local);
      overrideId = pose->data["id"].get<std::string>();
      resolve(tree, ctx);
    }
    const auto from =
        compose(inverse(tree.frames.at(target).root), tree.frames.at(selected->frame).root);
    rigidMatrix(from);
    const auto to = inverse(from);
    const auto fromNative = native(from, rotationError), toNative = native(to, rotationError);
    const V3 localCenter{0, 0, selected->anchor == "bottom" ? selected->size[2] / 2 : 0};
    const V3 center = pose_detail::apply(from, localCenter);
    require(finite(center), "工作空间中心溢出");
    auto poseMapping = identity();
    const auto q = orientation(from);
    const auto rotation = poses::rotationMatrix(q);
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column)
        poseMapping[4 * row + column] = rotation[3 * row + column];
      poseMapping[4 * row + 3] = center[row] - poseMapping[4 * row + 2] * localCenter[2];
    }
    double actualError = 0;
    for (int corner = 0; corner < 8; ++corner) {
      V3 local{selected->size[0] * (corner & 1 ? 0.5 : -0.5),
               selected->size[1] * (corner & 2 ? 0.5 : -0.5),
               selected->anchor == "bottom" ? selected->size[2] * (corner & 4 ? 1 : 0)
                                            : selected->size[2] * (corner & 4 ? 0.5 : -0.5)};
      const auto expected = pose_detail::apply(from, local),
                 delivered = nativeApply(fromNative, local);
      actualError = std::max({actualError, distance(expected, delivered),
                              distance(pose_detail::apply(poseMapping, local), expected),
                              distance(nativeApply(toNative, expected), local),
                              distance(nativeApply(toNative, delivered), local)});
    }
    require(std::isfinite(actualError) && actualError <= translationError,
            "工作空间角点/往返float32误差超过maxTranslationError");
    auto region = poses::make3D(selected->id, target, center, q, selected->size);
    region.data.erase("confidence");
    region.data["pickable"] = false;
    region.data["attributes"] = {{"workspaceFrame", selected->frame}, {"anchor", selected->anchor}};
    std::string why;
    require(poses::validate(region, why), "工作空间体积输出无效：" + why);
    Json frames = Json::array(), workspaces = Json::array();
    for (const auto& entry : tree.frames)
      frames.push_back({{"id", entry.first},
                        {"parent", entry.second.parent},
                        {"transform", entry.second.local},
                        {"toRoot", entry.second.root}});
    for (const auto& workspace : tree.workspaces)
      workspaces.push_back({{"id", workspace.id},
                            {"frame", workspace.frame},
                            {"size", workspace.size},
                            {"anchor", workspace.anchor},
                            {"toRoot", tree.frames.at(workspace.frame).root}});
    Record metadata{"geometry.Workspaces3D.v1",
                    {{"schemaVersion", 1},
                     {"unit", "m"},
                     {"rootFrame", tree.root},
                     {"targetFrame", target},
                     {"selectedWorkspace", selected->id},
                     {"workspaceFrame", selected->frame},
                     {"format", format},
                     {"path", p.path("path").u8string()},
                     {"contentFingerprint", fingerprint(bytes)},
                     {"frames", std::move(frames)},
                     {"workspaces", std::move(workspaces)},
                     {"overrideApplied", in.has("overrideRegion")},
                     {"overrideId", overrideId},
                     {"nativeCornerError", actualError}}};
    require(values::boundedJson(metadata.data, why), "工作空间快照超出Record预算：" + why,
            "resource_limit");
    poll(ctx);
    out.set("region", Data::record(std::move(region)));
    out.set("fromWorkspace", Data::transform(fromNative));
    out.set("toWorkspace", Data::transform(toNative));
    out.set("workspace", Data::record(std::move(metadata)));
    return Status::Ok();
  } catch (const Failure& failure) {
    return failure.status;
  } catch (const YAML::Exception& error) {
    return Status::Error(Phase::Execute, "bad_input",
                         std::string("工作空间YAML解析失败：") + error.what(), "path");
  } catch (const Json::exception& error) {
    return Status::Error(Phase::Execute, "bad_input",
                         std::string("工作空间JSON解析失败：") + error.what(), "path");
  } catch (const std::bad_alloc&) {
    return Status::Error(Phase::Execute, "resource_limit", "工作空间加载内存不足", "path");
  }
}
} // namespace

void registerP3Workspace(Registry& registry) {
  auto op = descriptor("workspace.load", "三维工作空间加载",
                       "workspace calibration frame tree volume load yaml json");
  op.capabilities = {true, false, false};
  op.inputs = {withContract(record("overrideRegion", false), {{"recordType", poses::kPose3DType}})};
  op.outputs = {withContract(record("region"), {{"recordType", poses::kPose3DType}}),
                {"fromWorkspace", "Transform", "工作空间→目标帧", "原生float32刚体变换。", true},
                {"toWorkspace", "Transform", "目标帧→工作空间", "可接transform.apply。", true},
                withContract(record("workspace"), {{"recordType", "geometry.Workspaces3D.v1"}})};
  auto path = text("path", "工作空间文件");
  path.type = ParamType::Path;
  path.mode = "open";
  op.params = {
      path,
      choice("format", "文件格式", "lyflow-json-v1", {"lyflow-json-v1", "xyz-unversioned-yaml"}),
      text("workspaceId", "工作空间ID", "workspace"),
      text("targetFrame", "目标参考帧（空为文件根帧）"),
      choice("legacyUnit", "外部YAML单位声明", "unspecified", {"unspecified", "m"}),
      text("legacyRootFrame", "外部YAML根参考帧", "World"),
      number("maxTranslationError", "原生角点/往返误差上限", 1e-5, "m"),
      number("maxRotationError", "原生旋转系数量化误差上限", 1e-6)};
  op.doc =
      "加载显式版本帧树或显式选择的无版本外部workspace YAML，输出可消费体积Pose和双向Transform。底面锚点转换为标准体积中心；拒绝缺父帧、环、非刚体、未知协议及原生精度超限。每次读文件，无全局树副作用。";
  op.compute = &load;
  registry.addOperator(std::move(op));
}
} // namespace lyflow::ops
