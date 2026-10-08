// io.load_image / io.save_image。
// 文件读写不交给 cv::imread / imwrite：它们走 CRT 的窄字符串路径，中文路径在 ANSI 代码页下打不开
// （与 PCL 同一个坑，见 std-pointcloud 的 pcl_path.h）。这里用 std::filesystem 读写字节，
// 再交给 imdecode / imencode —— 路径问题整个绕开。
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "ops.h"
#include "params.h"

namespace lyflow::ops {
namespace {

std::string lowerExtension(const std::filesystem::path& p) {
  std::string ext = p.extension().u8string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

/// OpenCV 的 BGR(A) ↔ 公共模型的 RGB(A)（ADR-0026）。单通道原样。
cv::Mat swapRedBlue(const cv::Mat& m) {
  cv::Mat out;
  if (m.channels() == 3) {
    cv::cvtColor(m, out, cv::COLOR_BGR2RGB);
  } else if (m.channels() == 4) {
    cv::cvtColor(m, out, cv::COLOR_BGRA2RGBA);
  } else {
    out = m;
  }
  return out;
}

Status loadCompute(const Inputs& inputs, const ParamView& params, Outputs& outputs, ExecContext& ctx) {
  if (params.choice("source") == "inputs") {
    const Image* image = inputs.has("image") ? inputs.get("image").asImage() : nullptr;
    if (!image || !image->consistent()) {
      return Status::Error(Phase::Execute, "bad_input", "source=inputs 时需要宿主注入或上游连接 image", {}, "image");
    }
    cv::Mat m = cvx::view(*image), converted;
    const std::string& mode = params.choice("mode");
    if (mode == "gray" && image->channels != 1) {
      cv::cvtColor(m, converted, image->channels == 3 ? cv::COLOR_RGB2GRAY : cv::COLOR_RGBA2GRAY);
    } else if (mode == "color" && image->channels != 3) {
      cv::cvtColor(m, converted, image->channels == 1 ? cv::COLOR_GRAY2RGB : cv::COLOR_RGBA2RGB);
    } else {
      outputs.set("image", inputs.get("image"));
      return Status::Ok();
    }
    return img::putMat(outputs, "image", std::move(converted));
  }
  const std::filesystem::path file = params.path("path");
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    return Status::Error(Phase::Execute, "io", "文件不存在: " + file.u8string(), "path");
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return Status::Error(Phase::Execute, "io", "打不开: " + file.u8string(), "path");
  // 量好大小一次读完：istreambuf_iterator 逐字节走 streambuf、边读边扩容，36 MB 的图光读文件就要几百毫秒
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
  if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return Status::Error(Phase::Execute, "io", "读不完: " + file.u8string(), "path");
  }

  const std::string& mode = params.choice("mode");
  int flags = cv::IMREAD_UNCHANGED;
  if (mode == "gray") flags = cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH;
  if (mode == "color") flags = cv::IMREAD_COLOR | cv::IMREAD_ANYDEPTH;
  cv::Mat m = cv::imdecode(bytes, flags);
  if (m.empty()) {
    return Status::Error(Phase::Execute, "io",
                         "解不开这个图像文件（格式不支持或文件损坏）: " + file.u8string(), "path");
  }
  if (m.channels() == 2) {
    // 灰度 + alpha 的 PNG：公共模型没有两通道，丢掉 alpha 比拒绝更有用
    std::vector<cv::Mat> planes;
    cv::split(m, planes);
    m = planes[0];
    ctx.log(LogLevel::Warn, "两通道图像（灰度 + alpha）只保留了灰度");
  }
  m = swapRedBlue(cvx::toSupportedDepth(m));
  ctx.log(LogLevel::Info, "读入 " + std::to_string(m.cols) + "×" + std::to_string(m.rows) + "×" +
                              std::to_string(m.channels()));
  return img::putMat(outputs, "image", std::move(m));
}

/// 路径没变但文件被覆盖了也必须重算（同 io.load_pcd）。
std::string loadExternalKey(const ParamView& params) {
  if (params.choice("source") == "inputs") return {};
  const std::filesystem::path file = params.path("path");
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto mtime = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(mtime.time_since_epoch().count());
}

Status saveCompute(const Inputs& inputs, const ParamView& params, Outputs&, ExecContext& ctx) {
  const Image& image = *inputs.get("image").asImage();
  const std::filesystem::path file = params.path("path");
  const std::string ext = lowerExtension(file);
  if (ext.empty()) {
    return img::badParam("输出路径要带扩展名（.png / .jpg / .bmp / .tif）", "path");
  }
  if (image.depth == PixelDepth::F32 && ext != ".tif" && ext != ".tiff") {
    return img::badParam("f32 图像只能存 .tif；要存 PNG 先接 image.normalize 转成 u8 / u16", "path");
  }
  std::vector<unsigned char> bytes;
  try {
    const cv::Mat bgr = swapRedBlue(cvx::view(image));
    if (!cv::imencode(ext, bgr, bytes)) {
      return img::badParam("OpenCV 编不出 " + ext + "（这种格式不支持这个位深 / 通道数）", "path");
    }
  } catch (const cv::Exception& e) {
    return img::fromCvError(e, "path");
  }
  std::error_code ec;
  if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return Status::Error(Phase::Execute, "io", "打不开输出文件: " + file.u8string(), "path");
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) return Status::Error(Phase::Execute, "io", "写入失败: " + file.u8string(), "path");
  ctx.log(LogLevel::Info, "写出 " + std::to_string(bytes.size()) + " 字节 -> " + file.u8string());
  return Status::Ok();
}

}  // namespace

void registerIoLoadImage(Registry& r) {
  OperatorDesc op;
  op.id = "io.load_image";
  op.version = "1.0.0";
  op.label = "加载图像";
  op.category = "输入输出/输入";
  op.keywords = {"load", "read", "open", "image", "png", "jpg", "tif", "读取", "图像", "图片"};
  op.doc = "从磁盘读取图像（PNG / JPEG / BMP / TIFF）。通道顺序转成 RGB(A)；16 位 PNG / TIFF 保留 u16。";
  op.doc += " source=inputs 时使用宿主经 RunImageInput 注入或上游连接的 image，不读文件。";
  op.inputs = {Port{"image", "Image", "Image", "source=inputs 时使用的图像。", false}};
  op.outputs = {img::imageOut("image", "读出的图像。")};

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "图像文件路径。相对路径相对于当前图文件所在目录。";
  path.def = Value::text("");
  path.mode = "open";
  path.visibleWhen = img::when("source", Value::text("file"));
  path.filters = {FileFilter{"Image", {"png", "jpg", "jpeg", "bmp", "tif", "tiff"}},
                  FileFilter{"All Files", {"*"}}};

  op.params = {
      img::enumParam("source", "Source", "file",
                     {EnumOption{"file", "文件", "从 path 读取图像"},
                      EnumOption{"inputs", "输入", "上游连接或宿主内存注入"}}, "图像来源。"),
      path,
      img::enumParam("mode", "Mode", "unchanged",
                     {EnumOption{"unchanged", "原样", "文件里是什么通道、什么位深就是什么"},
                      EnumOption{"gray", "灰度", "读成单通道，保留 16 位"},
                      EnumOption{"color", "彩色", "读成三通道 RGB，保留 16 位"}},
                     "读成什么形状。"),
  };
  // imdecode 进去就出不来 —— cancellable=false 是如实申报
  op.capabilities = {false, false, true};
  op.compute = &loadCompute;
  op.externalKey = &loadExternalKey;
  r.addOperator(std::move(op));
}

void registerIoSaveImage(Registry& r) {
  OperatorDesc op;
  op.id = "io.save_image";
  op.version = "1.0.0";
  op.label = "保存图像";
  op.category = "输入输出/输出";
  op.keywords = {"save", "write", "export", "image", "png", "保存", "导出", "图像"};
  op.doc = "把图像写到磁盘，按扩展名选格式。u8 / u16 存 PNG；f32 只能存 TIFF。";
  op.inputs = {img::imageIn("image", "要保存的图像。")};

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "输出路径。相对路径相对于当前图文件所在目录。";
  path.def = Value::text("");
  path.mode = "save";
  path.filters = {FileFilter{"PNG", {"png"}}, FileFilter{"JPEG", {"jpg", "jpeg"}},
                  FileFilter{"TIFF", {"tif", "tiff"}}, FileFilter{"BMP", {"bmp"}}};
  op.params = {path};
  // 没有输出端口的算子永远不 skipped（执行器按输出端口查缓存），与 io.save_pcd 同一个申报
  op.capabilities = {false, false, true};
  op.compute = &saveCompute;
  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops
