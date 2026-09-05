#include <pcl/PCLPointCloud2.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>

#include <algorithm>
#include <cctype>
#include <system_error>

#include "ops/ops.h"
#include "ops/pcl/adapter.h"
#include "ops/pcl/pcl_path.h"

namespace lyflow::ops {
namespace {

std::string lowerExtension(const std::filesystem::path& p) {
  std::string ext = p.extension().u8string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

Status compute(const Inputs&, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  const std::filesystem::path file = params.path("path");
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    return Status::Error(Phase::Execute, "io", "文件不存在: " + file.u8string(), "path");
  }

  // 中文路径的三种走法都藏在 PclPath 里（见 pcl_path.h 的长注释）。
  // 算子这一侧只需要「拿到一个 PCL 一定能打开的窄字符串」。
  io::PclPath narrow(file, /*forWrite=*/false);
  if (!narrow.ok()) {
    return Status::Error(Phase::Execute, "io", narrow.error(), "path");
  }

  // 走 PCLPointCloud2 而不是具体点类型：文件里到底有哪些通道要运行时才知道，
  // 用模板点类型就得为每种组合实例化一遍（见 adapter.h）。
  pcl::PCLPointCloud2 blob;
  const std::string ext = lowerExtension(file);
  int rc = -1;
  try {
    if (ext == ".ply") {
      rc = pcl::io::loadPLYFile(narrow.str(), blob);
    } else {
      rc = pcl::io::loadPCDFile(narrow.str(), blob);
    }
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "io",
                         std::string("读取失败: ") + e.what(), "path");
  }
  if (rc < 0) {
    return Status::Error(Phase::Execute, "io",
                         "无法解析点云文件（格式不支持或文件损坏）: " + file.u8string(), "path");
  }

  PointCloud cloud = adapter::fromBlob(blob);
  // width 和 height 都是 uint32，直接相乘会溢出（65536×65536 正好回绕成 0），
  // 于是「文件里没有 x/y/z」会被当成「文件是空的」而静默通过。
  const std::uint64_t declared =
      static_cast<std::uint64_t>(blob.width) * static_cast<std::uint64_t>(blob.height);
  if (cloud.pointCount() == 0 && declared > 0) {
    return Status::Error(Phase::Execute, "io", "文件里没有 x/y/z 字段: " + file.u8string(),
                         "path");
  }
  if (ctx.cancelled()) return Status::Ok();

  if (params.flag("recenter") && cloud.pointCount() > 0) {
    double cx = 0, cy = 0, cz = 0;
    const std::size_t n = cloud.pointCount();
    for (std::size_t i = 0; i < n; ++i) {
      cx += cloud.xyz[i * 3];
      cy += cloud.xyz[i * 3 + 1];
      cz += cloud.xyz[i * 3 + 2];
    }
    cx /= static_cast<double>(n);
    cy /= static_cast<double>(n);
    cz /= static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
      cloud.xyz[i * 3] -= static_cast<float>(cx);
      cloud.xyz[i * 3 + 1] -= static_cast<float>(cy);
      cloud.xyz[i * 3 + 2] -= static_cast<float>(cz);
    }
  }

  ctx.log(LogLevel::Info, "读入 " + std::to_string(cloud.pointCount()) + " 个点");
  outputs.set("cloud", Data::cloud(std::move(cloud)));
  return Status::Ok();
}

/// cacheKey 的外部状态钩子：路径没变但文件被覆盖了，也必须重算。
/// M3 打开缓存复用时，这一行就是「换了个 pcd 但忘了改参数」不会拿到旧结果的保证。
std::string externalKey(const ParamView& params) {
  const std::filesystem::path file = params.path("path");
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" +
         std::to_string(mtime.time_since_epoch().count());
}

}  // namespace

void registerIoLoadPcd(Registry& r) {
  OperatorDesc op;
  op.id = "io.load_pcd";
  op.version = "1.0.0";
  op.label = "Load PCD";
  op.category = "IO/Input";
  op.keywords = {"load", "read", "open", "pcd", "ply", "读取", "载入"};
  op.doc = "从磁盘读取点云文件。支持 PCD 与 PLY，按文件里实际存在的字段带出强度/法线/颜色通道。";

  // 无输入端口：这是一个源节点。
  op.outputs = {
      Port{"cloud", "PointCloud", "Cloud", "读出的点云。", true},
  };

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "点云文件路径。相对路径相对于当前图文件所在目录。";
  path.def = Value::text("");
  path.mode = "open";
  path.filters = {
      FileFilter{"Point Cloud", {"pcd", "ply"}},
      FileFilter{"All Files", {"*"}},
  };

  Param recenter;
  recenter.name = "recenter";
  recenter.type = ParamType::Bool;
  recenter.label = "Recenter To Origin";
  recenter.doc = "读入后把质心平移到原点。调试视角时方便，生产流程一般关掉。";
  recenter.def = Value::boolean(false);
  recenter.advanced = true;

  op.params = {path, recenter};

  // PCL 的读盘调用一旦进去就出不来，中途没有轮询点 —— cancellable=false
  // 是如实申报，不是偷懒：前端据此知道这个节点按 Esc 不会立刻停。
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;
  op.externalKey = &externalKey;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
