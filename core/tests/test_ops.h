#pragma once
// core 测试专用的算子（S6 / ADR-0014）。core 只自带 gen.synthetic 与 util.reroute，
// 执行器/缓存/子图要的那些「形状」由这里的 test.* 提供，不依赖任何算子包。
#include <atomic>
#include <set>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::test {
namespace ops {

// ------------------------------------------------------------ test.block
inline std::atomic<bool>& blockEntered() {
  static std::atomic<bool> flag{false};
  return flag;
}

inline Status blockCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                           ExecContext& ctx) {
  blockEntered().store(true);
  while (!ctx.cancelled()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 返回 Ok：执行器看的是 cancelled 标志而不是返回值（operator.h 的约定）
  outputs.set("cloud", inputs.get("cloud"));
  return Status::Ok();
}

// ------------------------------------------------------------ test.any_pass
inline Status anyPassCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                             ExecContext&) {
  outputs.set("out", inputs.get("in"));
  return Status::Ok();
}

inline Status noOutputCompute(const Inputs&, const ParamView&, Outputs&, ExecContext&) {
  return Status::Ok();
}

// ------------------------------------------------------------ test.sleep
inline Status sleepCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                           ExecContext& ctx) {
  using Ms = std::chrono::milliseconds;
  const auto deadline = std::chrono::steady_clock::now() + Ms(params.integer("ms"));
  while (std::chrono::steady_clock::now() < deadline && !ctx.cancelled()) {
    std::this_thread::sleep_for(Ms(2));
  }
  outputs.set("cloud", inputs.get("cloud"));
  return Status::Ok();
}

// ------------------------------------------------------------ test.thin
/// leaf 越大留下的点越少 —— 体素降采样的「形状」，但没有任何几何。
inline std::size_t strideOf(const std::array<float, 3>& leaf) {
  const double s = std::lround(static_cast<double>(leaf[0]) / 0.01);
  return s < 1.0 ? 1u : static_cast<std::size_t>(s);
}

inline Status thinCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                          ExecContext& ctx) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const std::size_t stride = strideOf(params.vec3("leaf"));
  std::vector<std::int32_t> keep;
  keep.reserve(cloud.pointCount() / stride + 1);
  Ticker ticker(ctx, cloud.pointCount());
  for (std::size_t i = 0; i < cloud.pointCount(); i += stride) {
    if (ticker.tick(i)) return Status::Ok();
    keep.push_back(static_cast<std::int32_t>(i));
  }
  outputs.set("cloud", Data::cloud(cloud.select(keep)));
  return Status::Ok();
}

// ------------------------------------------------------------ test.merge2
inline Status mergeCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                           ExecContext&) {
  const PointCloud& a = *inputs.get("a").asCloud();
  PointCloud out;
  out.xyz = a.xyz;
  out.intensity = a.intensity;
  // b 是可选输入：没连时原样输出 a
  if (inputs.has("b")) {
    const PointCloud& b = *inputs.get("b").asCloud();
    out.xyz.insert(out.xyz.end(), b.xyz.begin(), b.xyz.end());
    if (a.hasIntensity() && b.hasIntensity()) {
      out.intensity.insert(out.intensity.end(), b.intensity.begin(), b.intensity.end());
    } else {
      out.intensity.clear();
    }
  }
  outputs.set("cloud", Data::cloud(std::move(out)));
  return Status::Ok();
}

// ------------------------------------------------------------ test.half_indices
inline Status halfIndicesCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                                 ExecContext&) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  Indices out;
  out.sourceCloudId = cloud.id;
  for (std::size_t i = 0; i < cloud.pointCount() / 2; ++i) {
    out.values.push_back(static_cast<std::int32_t>(i));
  }
  outputs.set("indices", Data::indices(std::move(out)));
  return Status::Ok();
}

// ------------------------------------------------------------ test.split
inline Status splitCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                           ExecContext&) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const Indices& indices = *inputs.get("indices").asIndices();
  if (indices.sourceCloudId != cloud.id) {
    return Status::Error(Phase::Execute, "bad_input", "这组下标不是从当前输入点云上取的", {},
                         "indices");
  }
  outputs.set("selected", Data::cloud(cloud.select(indices.values)));
  outputs.set("rest", Data::cloud(cloud.selectInverse(indices.values)));
  return Status::Ok();
}

// ------------------------------------------------------------ test.sink
inline Status sinkCompute(const Inputs& inputs, const ParamView& params, Outputs&,
                          ExecContext&) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const std::filesystem::path file = params.path("path");
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return Status::Error(Phase::Execute, "io", "打不开 " + file.u8string(), "path");
  out << cloud.pointCount();
  return Status::Ok();
}

// ------------------------------------------------------------ test.migrated
inline Status migratedCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs,
                              ExecContext&) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const auto want = static_cast<std::size_t>(params.integer("keepCount"));
  std::vector<std::int32_t> keep;
  for (std::size_t i = 0; i < cloud.pointCount() && keep.size() < want; ++i) {
    keep.push_back(static_cast<std::int32_t>(i));
  }
  outputs.set("cloud", Data::cloud(cloud.select(keep)));
  return Status::Ok();
}

inline nlohmann::json migrateV1(const nlohmann::json& params) {
  nlohmann::json out = params;
  if (out.contains("count")) {
    out["keepCount"] = out["count"];
    out.erase("count");
  }
  return out;
}

// ------------------------------------------------------------ test.counted
/// 惰性调度的锁：备用闭包没被 demand 时，这个计数必须一动不动（A1 验收）。
inline std::atomic<int>& computeCalls() {
  static std::atomic<int> calls{0};
  return calls;
}

inline Status countedCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                             ExecContext&) {
  computeCalls().fetch_add(1, std::memory_order_relaxed);
  PointCloud cloud;
  const auto n = static_cast<std::size_t>(params.integer("pointCount"));
  cloud.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const auto f = static_cast<float>(i);
    cloud.push(f, f, f);
  }
  outputs.set("cloud", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

// ------------------------------------------------------------ test.validated
/// 加载期校验（m7-plan J5）的样板：mode=pinned 却没接 ref 是 error，loud=true 给一条 warning。
inline Status validatedCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                               ExecContext&) {
  outputs.set("cloud", inputs.get("cloud"));
  return Status::Ok();
}

inline std::vector<Issue> validatedCheck(const ParamView& params,
                                         const std::set<std::string>& connected) {
  std::vector<Issue> issues;
  if (params.text("mode") == "pinned" && !connected.count("ref")) {
    issues.push_back(Issue::error("bad_param", "pinned 必须接 ref", "mode", "ref"));
  }
  if (params.flag("loud")) {
    issues.push_back(Issue::warning("loud", "loud 打开了", "loud"));
  }
  return issues;
}

// ------------------------------------------------------------ test.fail
inline Status failCompute(const Inputs&, const ParamView& params, Outputs&, ExecContext&) {
  return Status::Error(Phase::Execute, params.text("code"), params.text("message"));
}

// ---------------------------------------------- 端口契约（ADR-0024）用的几个源
/// 带一个可选 NaN 的点云。契约的 finite 一条只有它喂得出反例。
inline Status nanCloudCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                              ExecContext&) {
  PointCloud cloud;
  const auto n = static_cast<std::size_t>(params.integer("pointCount"));
  const auto badAt = params.integer("badAt");
  cloud.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const auto f = static_cast<float>(i);
    if (badAt >= 0 && static_cast<std::size_t>(badAt) == i) {
      cloud.push(std::numeric_limits<float>::quiet_NaN(), f, f);
    } else {
      cloud.push(f, f, f);
    }
  }
  outputs.set("cloud", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

inline Status makeTensorCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                                ExecContext&) {
  Tensor t;
  for (const char* key : {"dim0", "dim1", "dim2"}) {
    const auto d = params.integer(key);
    if (d > 0) t.shape.push_back(d);
  }
  t.data.assign(t.elementCount(), 1.0F);
  if (params.flag("injectNaN") && !t.data.empty()) {
    t.data.front() = std::numeric_limits<float>::quiet_NaN();
  }
  outputs.set("tensor", Data::tensor(std::move(t)));
  return Status::Ok();
}

inline Status makeRecordCompute(const Inputs&, const ParamView& params, Outputs& outputs,
                                ExecContext&) {
  Record rec;
  rec.type = params.text("type");
  rec.data["n"] = 1;
  outputs.set("rec", Data::record(std::move(rec)));
  return Status::Ok();
}

/// 四种契约各一个端口。只有 count 是必填，其余按用例接不接。
inline Status contractedCompute(const Inputs& inputs, const ParamView&, Outputs& outputs,
                                ExecContext&) {
  outputs.set("cloud", inputs.get("count"));
  return Status::Ok();
}

inline Param textParam(const char* name, const char* def) {
  Param p;
  p.name = name;
  p.type = ParamType::String;
  p.label = name;
  p.def = Value::text(def);
  return p;
}

inline Param boolParam(const char* name, bool def) {
  Param p;
  p.name = name;
  p.type = ParamType::Bool;
  p.label = name;
  p.def = Value::boolean(def);
  return p;
}

inline Param intParam(const char* name, std::int64_t def, double min) {
  Param p;
  p.name = name;
  p.type = ParamType::Int;
  p.label = name;
  p.def = Value::integer(def);
  p.min = min;
  return p;
}

}  // namespace ops

/// 把 test.* 全部注册进进程内那份注册表。多次调用只生效一次。
inline void ensureTestOps() {
  static std::once_flag once;
  std::call_once(once, [] {
    Registry& r = ensureRegistry();
    const Port cloudIn{"cloud", "PointCloud", "Cloud", "", true};
    const Port cloudOut{"cloud", "PointCloud", "Cloud", "", true};

    {  // 一直阻塞到收到取消为止：让「运行中取消」成为确定性事件
      OperatorDesc op;
      op.id = "test.block";
      op.version = "1.0.0";
      op.label = "Block Until Cancelled";
      op.category = "Test";
      op.doc = "只在测试里注册：一直阻塞到收到取消为止。";
      op.inputs = {cloudIn};
      op.outputs = {cloudOut};
      op.capabilities = {true, false, false};
      op.compute = &ops::blockCompute;
      r.addOperator(std::move(op));
    }
    {  // Any → Any：「声明类型全通过、实际载荷对不上」这条路径的唯一入口
      OperatorDesc op;
      op.id = "test.any_pass";
      op.version = "1.0.0";
      op.label = "Any Passthrough";
      op.category = "Test";
      op.doc = "只在测试里注册：Any 进 Any 出的透传节点。";
      op.inputs = {Port{"in", "Any", "In", "", true}};
      op.outputs = {Port{"out", "Any", "Out", "", true}};
      op.capabilities = {true, false, true};
      op.compute = &ops::anyPassCompute;
      r.addOperator(std::move(op));
    }
    {
      OperatorDesc op;
      op.id = "test.no_output";
      op.version = "1.0.0";
      op.label = "No Output";
      op.category = "Test";
      op.doc = "只在测试里注册：声明了输出端口却一个都不写。";
      op.inputs = {cloudIn};
      op.outputs = {cloudOut};
      op.capabilities = {false, false, true};
      op.compute = &ops::noOutputCompute;
      r.addOperator(std::move(op));
    }
    {  // 并行验收要的是墙钟时间这一个可观测量，真算子的耗时随机器浮动
      OperatorDesc op;
      op.id = "test.sleep";
      op.version = "1.0.0";
      op.label = "Sleep";
      op.category = "Test";
      op.doc = "只在测试里注册：睡一会儿再把输入原样传出去。";
      op.inputs = {cloudIn};
      op.outputs = {cloudOut};
      // tag 只为让并行的几支拿到不同的 cacheKey —— 参数一样的话第二支就命中缓存了
      Param tag;
      tag.name = "tag";
      tag.type = ParamType::String;
      tag.label = "Tag";
      tag.def = Value::text("");
      op.params = {ops::intParam("ms", 200, 0.0), tag};
      op.capabilities = {true, false, true};
      op.compute = &ops::sleepCompute;
      r.addOperator(std::move(op));
    }
    {  // 降采样的形状：leaf 越大留下的点越少，leaf 非法就在校验期红
      OperatorDesc op;
      op.id = "test.thin";
      op.version = "1.0.0";
      op.label = "Thin";
      op.category = "Test";
      op.doc = "只在测试里注册：按 leaf 换算出的步长抽稀。";
      op.inputs = {cloudIn};
      op.outputs = {cloudOut};
      Param leaf;
      leaf.name = "leaf";
      leaf.type = ParamType::Vec3f;
      leaf.label = "Leaf Size";
      leaf.def = Value::vec({0.02, 0.02, 0.02});
      leaf.min = 0.0001;
      leaf.unit = "m";
      op.params = {leaf};
      op.capabilities = {true, true, true};
      op.compute = &ops::thinCompute;
      r.addOperator(std::move(op));
    }
    {  // 两个输入端口：环检测与单连接规则都要它
      OperatorDesc op;
      op.id = "test.merge2";
      op.version = "1.0.0";
      op.label = "Merge Two";
      op.category = "Test";
      op.doc = "只在测试里注册：把两片点云首尾相接。b 可选，用来测「可选输入没连不报错」。";
      op.inputs = {Port{"a", "PointCloud", "A", "", true},
                   Port{"b", "PointCloud", "B", "", false}};
      op.outputs = {cloudOut};
      op.capabilities = {true, true, true};
      op.compute = &ops::mergeCompute;
      r.addOperator(std::move(op));
    }
    {  // 唯一一个产出 Indices 的测试算子
      OperatorDesc op;
      op.id = "test.half_indices";
      op.version = "1.0.0";
      op.label = "Half Indices";
      op.category = "Test";
      op.doc = "只在测试里注册：取前一半点的下标。";
      op.inputs = {cloudIn};
      op.outputs = {Port{"indices", "Indices", "Indices", "", true}};
      op.capabilities = {true, true, true};
      op.compute = &ops::halfIndicesCompute;
      r.addOperator(std::move(op));
    }
    {  // cloud + indices → 两片云：静音找不到同类型源、下标对不上账都要它
      OperatorDesc op;
      op.id = "test.split";
      op.version = "1.0.0";
      op.label = "Split By Indices";
      op.category = "Test";
      op.doc = "只在测试里注册：按下标把点云一分为二。";
      op.inputs = {cloudIn, Port{"indices", "Indices", "Indices", "", true}};
      op.outputs = {Port{"selected", "PointCloud", "Selected", "", true},
                    Port{"rest", "PointCloud", "Rest", "", true}};
      op.capabilities = {true, true, true};
      op.compute = &ops::splitCompute;
      r.addOperator(std::move(op));
    }
    {  // 没有输出端口 → 纯副作用，永远不该被缓存跳过
      OperatorDesc op;
      op.id = "test.sink";
      op.version = "1.0.0";
      op.label = "Sink";
      op.category = "Test";
      op.doc = "只在测试里注册：把点数写进一个文件。";
      op.inputs = {cloudIn};
      Param path;
      path.name = "path";
      path.type = ParamType::Path;
      path.label = "File";
      path.def = Value::text("");
      path.mode = "save";
      op.params = {path};
      op.capabilities = {false, false, true};
      op.compute = &ops::sinkCompute;
      r.addOperator(std::move(op));
    }
    {  // 主版本 2，带一条 1 → 2 的参数改名迁移（ADR-0008）
      OperatorDesc op;
      op.id = "test.migrated";
      op.version = "2.0.0";
      op.label = "Migrated";
      op.category = "Test";
      op.doc = "只在测试里注册：v1 的 count 在 v2 改名成 keepCount。";
      op.inputs = {cloudIn};
      op.outputs = {cloudOut};
      op.params = {ops::intParam("keepCount", 1000, 0.0), ops::intParam("seed", 0, 0.0)};
      op.capabilities = {true, true, true};
      op.compute = &ops::migratedCompute;
      op.migrations = {Migration{1, &ops::migrateV1}};
      r.addOperator(std::move(op));
    }
    {  // 数自己被调了几次。惰性端口的「没被 demand 就绝不调 compute」全靠它钉住
      OperatorDesc op;
      op.id = "test.counted";
      op.version = "1.0.0";
      op.label = "Counted Source";
      op.category = "Test";
      op.doc = "只在测试里注册：产一片点云，并把自己被调用的次数记进全局计数器。";
      op.outputs = {cloudOut};
      op.params = {ops::intParam("pointCount", 8, 0.0), ops::intParam("seed", 0, 0.0)};
      op.capabilities = {false, false, true};
      op.compute = &ops::countedCompute;
      r.addOperator(std::move(op));
    }
    // -------------------------------------------- 端口契约（ADR-0024）
    {  // 带一个可选 NaN 的源：finite 那一条只有它喂得出反例
      OperatorDesc op;
      op.id = "test.nonfinite_cloud";
      op.version = "1.0.0";
      op.label = "NaN Cloud";
      op.category = "Test";
      op.doc = "只在测试里注册：产一片点云，badAt >= 0 时把那一个点的 x 写成 NaN。";
      op.outputs = {cloudOut};
      op.params = {ops::intParam("pointCount", 6, 0.0), ops::intParam("badAt", -1, -1.0)};
      op.capabilities = {false, false, true};
      op.compute = &ops::nanCloudCompute;
      r.addOperator(std::move(op));
    }
    {
      OperatorDesc op;
      op.id = "test.make_tensor";
      op.version = "1.0.0";
      op.label = "Make Tensor";
      op.category = "Test";
      op.doc = "只在测试里注册：按 dim0/dim1/dim2 产一个全 1 的张量，dim<=0 的那一维不要。";
      op.outputs = {Port{"tensor", "Tensor", "Tensor", "", true}};
      op.params = {ops::intParam("dim0", 2, 0.0), ops::intParam("dim1", 3, 0.0),
                   // 参数名里不能出现小写的 n-a-n：manifest 序列化用例是子串匹配
                   // （test_executor.cpp:270）
                   ops::intParam("dim2", 4, 0.0), ops::boolParam("injectNaN", false)};
      op.capabilities = {false, false, true};
      op.compute = &ops::makeTensorCompute;
      r.addOperator(std::move(op));
    }
    {
      OperatorDesc op;
      op.id = "test.make_record";
      op.version = "1.0.0";
      op.label = "Make Record";
      op.category = "Test";
      op.doc = "只在测试里注册：产一个指定 type 的 Record。";
      op.outputs = {Port{"rec", "Record", "Record", "", true}};
      op.params = {ops::textParam("type", "Wanted")};
      op.capabilities = {false, false, true};
      op.compute = &ops::makeRecordCompute;
      r.addOperator(std::move(op));
    }
    {  // 四种契约各一个端口。uncontracted 是同一组端口去掉 contract 的对照组：
       // 「没声明就一行检查都不跑」这条只能靠一个对照组钉住。
      const Port count{"count", "PointCloud", "Count", "", true};
      const Port finite{"finite", "PointCloud", "Finite", "", false};
      const Port tensor{"tensor", "Tensor", "Tensor", "", false};
      const Port rec{"rec", "Record", "Record", "", false};
      auto make = [&](const char* id, const char* doc, std::vector<Port> inputs) {
        OperatorDesc op;
        op.id = id;
        op.version = "1.0.0";
        op.label = "Contracted Inputs";
        op.category = "Test";
        op.doc = doc;
        op.inputs = std::move(inputs);
        op.outputs = {cloudOut};
        op.capabilities = {false, false, true};
        op.compute = &ops::contractedCompute;
        r.addOperator(std::move(op));
      };
      make("test.contracted", "只在测试里注册：四种端口契约各一条。",
           {withContract(count, {{"elementCount", {{"eq", 6}}}}),
            withContract(finite, {{"finite", true}}),
            withContract(tensor, {{"shape", {2, -1, 4}}}),
            withContract(rec, {{"recordType", "Wanted"}})});
      make("test.uncontracted",
           "只在测试里注册：与 test.contracted 同一组端口，但一条契约都不声明。",
           {count, finite, tensor, rec});
    }
    {  // 加载期校验的钩子（J5）
      OperatorDesc op;
      op.id = "test.validated";
      op.version = "1.0.0";
      op.label = "Validated";
      op.category = "Test";
      op.doc = "只在测试里注册：带一个 validate 钩子的透传节点。";
      op.inputs = {cloudIn, Port{"ref", "PointCloud", "Ref", "", false}};
      op.outputs = {cloudOut};
      op.params = {ops::textParam("mode", "free"), ops::boolParam("loud", false)};
      op.capabilities = {false, false, true};
      op.compute = &ops::validatedCompute;
      op.validate = &ops::validatedCheck;
      r.addOperator(std::move(op));
    }
    {  // 一定失败：acceptsError 与 upstream_failed 两条路都要它
      OperatorDesc op;
      op.id = "test.fail";
      op.version = "1.0.0";
      op.label = "Always Fails";
      op.category = "Test";
      op.doc = "只在测试里注册：按参数报一条错误。";
      op.outputs = {cloudOut};
      op.params = {ops::textParam("code", "io"), ops::textParam("message", "测试用的失败")};
      op.capabilities = {false, false, true};
      op.compute = &ops::failCompute;
      r.addOperator(std::move(op));
    }
  });
}

}  // namespace lyflow::test
