#pragma once
// cacheKey 的哈希。XXH3-128：非加密但 128 位抗碰撞足够，快到编译一张图时可以忽略。
// 单头 vendored（D7），XXH_INLINE_ALL 引入，不需要额外的 .c。
#include <cstdint>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

namespace lyflow::exec {

/// 把若干段字节喂进去，得到 32 位十六进制串。
/// 每段之间插一个 '\0' 分隔：不这样的话 ("ab","c") 和 ("a","bc") 会同键。
class Hasher {
 public:
  void add(const std::string& s) {
    buffer_.insert(buffer_.end(), s.begin(), s.end());
    buffer_.push_back('\0');
  }

  std::string hex() const {
    const XXH128_hash_t h = XXH3_128bits(buffer_.data(), buffer_.size());
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int half = 0; half < 2; ++half) {
      const std::uint64_t v = half == 0 ? h.high64 : h.low64;
      for (int i = 15; i >= 0; --i) {
        out.push_back(kDigits[(v >> (i * 4)) & 0xF]);
      }
    }
    return out;
  }

 private:
  std::vector<char> buffer_;
};

}  // namespace lyflow::exec
