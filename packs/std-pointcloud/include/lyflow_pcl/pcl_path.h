#pragma once
// 中文路径（D9 的执行面）。PCL 的文件 IO 落在 CRT 的窄字符串接口上，
// 所以开机探一次窄字符串能不能表示 UTF-8，结论决定后续 IO 走哪条路（core/README.md）。
#include <filesystem>
#include <string>

namespace lyflow::ops::io {

enum class NarrowMode {
  Utf8,      ///< 窄字符串按 UTF-8 解释，直接传
  Acp,       ///< 按 ANSI 代码页转一次（GBK 系统上的中文路径走这条）
  TempCopy,  ///< 两条都不行，借道一个纯 ASCII 的临时文件
};

/// 探测结果。首次调用时做一次真实的文件读写探测，之后走缓存。
NarrowMode narrowMode();

/// 给 PCL 用的路径。构造即准备好，析构负责清理临时文件。
class PclPath {
 public:
  /// forWrite=false 表示要读这个文件（必要时先把它拷成临时文件）
  /// forWrite=true 表示要写（必要时先写临时文件，再由 commit 搬到目标位置）
  PclPath(const std::filesystem::path& target, bool forWrite);
  ~PclPath();

  PclPath(const PclPath&) = delete;
  PclPath& operator=(const PclPath&) = delete;

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  /// 传给 PCL 的窄字符串
  const std::string& str() const { return narrow_; }

  /// 写模式：把临时文件搬到目标位置。读模式：无操作。失败时填 error()。
  bool commit();

 private:
  std::filesystem::path target_;
  std::filesystem::path temp_;
  std::string narrow_;
  std::string error_;
  bool forWrite_ = false;
  bool ok_ = false;
  bool committed_ = false;
};

}  // namespace lyflow::ops::io
