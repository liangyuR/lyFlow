#include "ops/pcl/pcl_path.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace lyflow::ops::io {
namespace {

bool isAscii(const std::string& s) {
  for (unsigned char c : s) {
    if (c >= 0x80) return false;
  }
  return true;
}

#if defined(_WIN32)
/// UTF-8 → ANSI 代码页。无法无损表示时返回 false。
bool toAcp(const std::filesystem::path& p, std::string& out) {
  const std::wstring w = p.wstring();
  BOOL usedDefault = FALSE;
  const int len = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0, nullptr,
                                        &usedDefault);
  if (len <= 0) return false;
  std::string buf(static_cast<std::size_t>(len), '\0');
  ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), static_cast<int>(w.size()),
                        buf.data(), len, nullptr, &usedDefault);
  if (usedDefault) return false;  // 有字符落到了 '?'，用它去开文件必然打不开
  out = std::move(buf);
  return true;
}
#else
bool toAcp(const std::filesystem::path& p, std::string& out) {
  out = p.string();
  return true;
}
#endif

std::filesystem::path uniqueTempPath(const std::string& extension) {
  static std::atomic<std::uint64_t> counter{0};
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec) dir = std::filesystem::path(".");
  std::random_device rd;
  const std::uint64_t tag = (static_cast<std::uint64_t>(rd()) << 32) ^
                            counter.fetch_add(1, std::memory_order_relaxed);
  char name[64];
  std::snprintf(name, sizeof(name), "lyflow_%016llx", static_cast<unsigned long long>(tag));
  return dir / (std::string(name) + extension);
}

NarrowMode probe() {
  // 用宽字符 API 写一个带中文名的文件，再试着用窄字符串打开它。
  // 这里刻意用 std::ofstream/ifstream 而不是 Win32 API —— PCL 走的就是 CRT，
  // 探测必须走同一条路，不然探到的结论和实际用的接口对不上。
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec) return NarrowMode::TempCopy;

  const std::filesystem::path probePath =
      dir / std::filesystem::u8path(u8"lyflow_探测_中文.tmp");
  {
    std::ofstream out(probePath, std::ios::binary);
    if (!out) return NarrowMode::TempCopy;
    out << "probe";
  }
  struct Cleanup {
    const std::filesystem::path& p;
    ~Cleanup() {
      std::error_code e;
      std::filesystem::remove(p, e);
    }
  } cleanup{probePath};

  {
    std::ifstream in(probePath.u8string().c_str(), std::ios::binary);
    if (in.good()) return NarrowMode::Utf8;
  }
  std::string acp;
  if (toAcp(probePath, acp)) {
    std::ifstream in(acp.c_str(), std::ios::binary);
    if (in.good()) return NarrowMode::Acp;
  }
  return NarrowMode::TempCopy;
}

}  // namespace

NarrowMode narrowMode() {
  static NarrowMode mode = probe();
  return mode;
}

PclPath::PclPath(const std::filesystem::path& target, bool forWrite)
    : target_(target), forWrite_(forWrite) {
  const std::string utf8 = target.u8string();

  // 纯 ASCII 路径三种模式下都一样，直接短路 —— 绝大多数调用走这条。
  if (isAscii(utf8)) {
    narrow_ = utf8;
    ok_ = true;
    return;
  }

  switch (narrowMode()) {
    case NarrowMode::Utf8:
      narrow_ = utf8;
      ok_ = true;
      return;
    case NarrowMode::Acp:
      if (toAcp(target, narrow_)) {
        ok_ = true;
        return;
      }
      break;  // 表示不了，落到临时文件
    case NarrowMode::TempCopy:
      break;
  }

  temp_ = uniqueTempPath(target.extension().u8string());
  const std::string tempUtf8 = temp_.u8string();
  if (!isAscii(tempUtf8)) {
    error_ = "临时目录本身含非 ASCII 字符，无法为这个路径准备中转文件: " + tempUtf8;
    return;
  }
  narrow_ = tempUtf8;

  if (!forWrite) {
    std::error_code ec;
    std::filesystem::copy_file(target_, temp_,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      error_ = "读取 " + utf8 + " 失败: " + ec.message();
      return;
    }
  }
  ok_ = true;
}

bool PclPath::commit() {
  if (!ok_) return false;
  if (!forWrite_ || temp_.empty()) {
    committed_ = true;
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(target_.parent_path(), ec);
  std::filesystem::rename(temp_, target_, ec);
  if (ec) {
    // 跨卷时 rename 会失败，退回拷贝
    ec.clear();
    std::filesystem::copy_file(temp_, target_,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      error_ = "写入 " + target_.u8string() + " 失败: " + ec.message();
      return false;
    }
    std::filesystem::remove(temp_, ec);
  }
  committed_ = true;
  return true;
}

PclPath::~PclPath() {
  if (temp_.empty()) return;
  if (forWrite_ && committed_) return;
  std::error_code ec;
  std::filesystem::remove(temp_, ec);
}

}  // namespace lyflow::ops::io
