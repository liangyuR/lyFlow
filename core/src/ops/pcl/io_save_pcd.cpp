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

Status compute(const Inputs& inputs, const ParamView& params, Outputs&, ExecContext& ctx) {
  const PointCloud& cloud = *inputs.get("cloud").asCloud();
  const std::filesystem::path file = params.path("path");
  const Status s = saveCloudToFile(cloud, file, params.choice("format"));
  if (!s.ok) return s;
  ctx.log(LogLevel::Info,
          "写出 " + std::to_string(cloud.pointCount()) + " 个点 -> " + file.u8string());
  return Status::Ok();
}

}  // namespace

Status saveCloudToFile(const PointCloud& cloud, const std::filesystem::path& file,
                       const std::string& format) {
  std::error_code ec;
  if (!file.parent_path().empty()) {
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
      return Status::Error(Phase::Execute, "io",
                           "无法创建目录 " + file.parent_path().u8string() + ": " + ec.message(),
                           "path");
    }
  }

  // 写模式：目标路径含中文且当前进程的窄字符串走不通时，PclPath 会让 PCL
  // 先写一个纯 ASCII 的临时文件，commit() 再搬过去。
  io::PclPath narrow(file, /*forWrite=*/true);
  if (!narrow.ok()) {
    return Status::Error(Phase::Execute, "io", narrow.error(), "path");
  }

  pcl::PCLPointCloud2 blob;
  adapter::toBlob(cloud, blob);

  int rc = -1;
  try {
    if (lowerExtension(file) == ".ply") {
      rc = pcl::io::savePLYFile(narrow.str(), blob, Eigen::Vector4f::Zero(),
                                Eigen::Quaternionf::Identity(), format != "ascii");
    } else if (format == "ascii") {
      rc = pcl::io::savePCDFile(narrow.str(), blob, Eigen::Vector4f::Zero(),
                                Eigen::Quaternionf::Identity(), /*binary_mode=*/false);
    } else if (format == "binary_compressed") {
      // savePCDFile 的 binary_mode 走的是非压缩二进制，压缩要走 PCDWriter 的专门入口
      pcl::PCDWriter writer;
      rc = writer.writeBinaryCompressed(narrow.str(), blob);
    } else {
      rc = pcl::io::savePCDFile(narrow.str(), blob, Eigen::Vector4f::Zero(),
                                Eigen::Quaternionf::Identity(), /*binary_mode=*/true);
    }
  } catch (const std::exception& e) {
    return Status::Error(Phase::Execute, "io", std::string("写入失败: ") + e.what(), "path");
  }
  if (rc < 0) {
    return Status::Error(Phase::Execute, "io", "写入失败: " + file.u8string(), "path");
  }
  if (!narrow.commit()) {
    return Status::Error(Phase::Execute, "io", narrow.error(), "path");
  }
  return Status::Ok();
}

void registerIoSavePcd(Registry& r) {
  OperatorDesc op;
  op.id = "io.save_pcd";
  op.version = "1.0.0";
  op.label = "Save PCD";
  op.category = "IO/Output";
  op.keywords = {"save", "write", "export", "pcd", "ply", "保存", "导出"};
  op.doc = "把点云写到磁盘。按扩展名选 PCD 或 PLY，实际存在的通道都会写进去。";

  // 只有输入没有输出：这是一个 sink。执行器不会因为「没有输出端口」而报错，
  // 因为契约检查是按 op->outputs 逐个查的，空列表天然通过。
  op.inputs = {Port{"cloud", "PointCloud", "Cloud", "要保存的点云。", true}};

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "输出路径。相对路径相对于当前图文件所在目录。";
  path.def = Value::text("");
  path.mode = "save";
  path.filters = {
      FileFilter{"Point Cloud", {"pcd", "ply"}},
  };

  Param format;
  format.name = "format";
  format.type = ParamType::Enum;
  format.label = "Format";
  format.doc = "二进制最快，压缩二进制最小，ASCII 可以用文本编辑器打开看。";
  format.def = Value::text("binary");
  format.options = {
      EnumOption{"binary", "Binary", "未压缩二进制，读写最快。"},
      EnumOption{"binary_compressed", "Binary Compressed", "LZF 压缩，文件最小。"},
      EnumOption{"ascii", "ASCII", "纯文本，方便肉眼检查，文件最大。"},
  };

  op.params = {path, format};
  op.capabilities = {/*cancellable=*/false, /*previewable=*/false, /*deterministic=*/true};
  op.compute = &compute;

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
