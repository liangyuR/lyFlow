#include "exec/disk_cache.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#define LYFLOW_GETPID _getpid
#else
#include <unistd.h>
#define LYFLOW_GETPID getpid
#endif

namespace lyflow::exec {
namespace {

// 文件头：魔数 + 格式版本 + 端口数；每个端口：名字（u16 长度 + 字节）、种类（u8）、负载长度（u64）、负载。
// 本机字节序 —— 目录按构建指纹分，同一指纹只会在同一种机器上读写。
constexpr char kMagic[4] = {'L', 'F', 'C', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint8_t kKindCloud = 1;
constexpr std::uint8_t kKindTensor = 2;
constexpr std::uint8_t kHasIntensity = 1;
constexpr std::uint8_t kHasNormals = 2;
constexpr std::uint8_t kHasRgb = 4;

template <typename T>
void put(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
void putVec(std::string& out, const std::vector<T>& v) {
  if (!v.empty()) out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

struct Reader {
  const std::string& s;
  std::size_t at = 0;

  template <typename T>
  bool get(T& v) {
    if (s.size() - at < sizeof(T)) return false;
    std::memcpy(&v, s.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
  }
  template <typename T>
  bool getVec(std::vector<T>& v, std::uint64_t n) {
    if (n > (s.size() - at) / sizeof(T)) return false;
    v.resize(static_cast<std::size_t>(n));
    if (n) std::memcpy(v.data(), s.data() + at, static_cast<std::size_t>(n) * sizeof(T));
    at += static_cast<std::size_t>(n) * sizeof(T);
    return true;
  }
  bool bytes(std::string& out, std::size_t n) {
    if (s.size() - at < n) return false;
    out.assign(s.data() + at, n);
    at += n;
    return true;
  }
};

void encodeCloud(const PointCloud& c, std::string& out) {
  const std::uint64_t n = c.pointCount();
  std::uint8_t flags = 0;
  if (c.hasIntensity()) flags |= kHasIntensity;
  if (c.hasNormals()) flags |= kHasNormals;
  if (c.hasRgb()) flags |= kHasRgb;
  put(out, n);
  put(out, flags);
  putVec(out, c.xyz);
  putVec(out, c.intensity);
  putVec(out, c.normals);
  putVec(out, c.rgb);
}

bool decodeCloud(Reader& r, PointCloud& c) {
  std::uint64_t n = 0;
  std::uint8_t flags = 0;
  if (!r.get(n) || !r.get(flags)) return false;
  if (!r.getVec(c.xyz, n * 3)) return false;
  if ((flags & kHasIntensity) && !r.getVec(c.intensity, n)) return false;
  if ((flags & kHasNormals) && !r.getVec(c.normals, n * 3)) return false;
  if ((flags & kHasRgb) && !r.getVec(c.rgb, n * 3)) return false;
  return c.channelsConsistent();
}

void encodeTensor(const Tensor& t, std::string& out) {
  const std::uint32_t ndim = static_cast<std::uint32_t>(t.shape.size());
  put(out, ndim);
  putVec(out, t.shape);
  const std::uint64_t count = t.data.size();
  put(out, count);
  putVec(out, t.data);
}

bool decodeTensor(Reader& r, Tensor& t) {
  std::uint32_t ndim = 0;
  std::uint64_t count = 0;
  if (!r.get(ndim) || ndim > 16 || !r.getVec(t.shape, ndim)) return false;
  if (!r.get(count) || !r.getVec(t.data, count)) return false;
  return t.consistent();
}

/// 读文件头与端口表（不读负载内容）：名字、种类、负载在文件里的起点与长度。
struct PortEntry {
  std::string name;
  std::uint8_t kind = 0;
  std::size_t offset = 0;
  std::uint64_t length = 0;
};

bool readTable(Reader& r, std::vector<PortEntry>& out) {
  std::string magic;
  std::uint32_t version = 0, count = 0;
  if (!r.bytes(magic, 4) || std::memcmp(magic.data(), kMagic, 4) != 0) return false;
  if (!r.get(version) || version != kFormatVersion) return false;
  if (!r.get(count) || count > 256) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    PortEntry e;
    std::uint16_t nameLen = 0;
    if (!r.get(nameLen) || !r.bytes(e.name, nameLen)) return false;
    if (!r.get(e.kind) || !r.get(e.length)) return false;
    if (e.length > r.s.size() - r.at) return false;
    e.offset = r.at;
    r.at += static_cast<std::size_t>(e.length);
    out.push_back(std::move(e));
  }
  return r.at == r.s.size();
}

bool covers(const std::vector<PortEntry>& table, const std::vector<std::string>& ports) {
  for (const auto& p : ports) {
    bool found = false;
    for (const auto& e : table) found = found || e.name == p;
    if (!found) return false;
  }
  return !ports.empty();
}

bool readFile(const std::filesystem::path& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return static_cast<bool>(in) || in.eof();
}

/// cacheKey 本来就是十六进制；防一手别的字符进路径。
std::string safeName(const std::string& key) {
  std::string s = key;
  for (char& c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '-' || c == '_';
    if (!ok) c = '_';
  }
  return s;
}

}  // namespace

bool persistable(const Data& d) { return d.asCloud() != nullptr || d.asTensor() != nullptr; }

bool encodeNode(const PortData& ports, std::string& out) {
  if (ports.empty()) return false;
  for (const auto& [name, d] : ports) {
    if (!persistable(d) || name.size() > 0xFFFF) return false;
  }
  std::string buf(kMagic, 4);
  put(buf, kFormatVersion);
  put(buf, static_cast<std::uint32_t>(ports.size()));
  for (const auto& [name, d] : ports) {
    std::string payload;
    std::uint8_t kind = 0;
    if (const PointCloud* c = d.asCloud()) {
      kind = kKindCloud;
      encodeCloud(*c, payload);
    } else {
      kind = kKindTensor;
      encodeTensor(*d.asTensor(), payload);
    }
    put(buf, static_cast<std::uint16_t>(name.size()));
    buf += name;
    put(buf, kind);
    put(buf, static_cast<std::uint64_t>(payload.size()));
    buf += payload;
  }
  out = std::move(buf);
  return true;
}

bool decodeNode(const std::string& bytes, PortData& out) {
  Reader table{bytes};
  std::vector<PortEntry> entries;
  if (!readTable(table, entries)) return false;
  PortData ports;
  for (const PortEntry& e : entries) {
    const std::string payload = bytes.substr(e.offset, static_cast<std::size_t>(e.length));
    Reader r{payload};
    if (e.kind == kKindCloud) {
      PointCloud c;  // 构造时拿一个新的进程内 id —— Indices 对账靠它，不能沿用写盘那个进程的
      if (!decodeCloud(r, c) || r.at != payload.size()) return false;
      ports.emplace_back(e.name, Data::cloud(std::move(c)));
    } else if (e.kind == kKindTensor) {
      Tensor t;
      if (!decodeTensor(r, t) || r.at != payload.size()) return false;
      ports.emplace_back(e.name, Data::tensor(std::move(t)));
    } else {
      return false;
    }
  }
  out = std::move(ports);
  return true;
}

DiskCache& DiskCache::instance() {
  static DiskCache cache;
  return cache;
}

std::string DiskCache::setDir(const std::string& dir, const std::string& fingerprint) {
  std::lock_guard<std::mutex> lock(mu_);
  if (dir.empty()) {
    root_.clear();
    return std::string();
  }
  if (fingerprint.empty()) return "落盘缓存要一个构建指纹：没有它，改了算子实现也会读到旧结果";
  const std::filesystem::path root = std::filesystem::u8path(dir) / std::filesystem::u8path(safeName(fingerprint));
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec || !std::filesystem::is_directory(root, ec)) {
    root_.clear();
    return "建不出缓存目录 " + root.u8string() + (ec ? "：" + ec.message() : std::string());
  }
  root_ = root;
  return std::string();
}

bool DiskCache::enabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !root_.empty();
}

void DiskCache::setMinDurationMs(double ms) {
  std::lock_guard<std::mutex> lock(mu_);
  minDurationMs_ = ms;
}

double DiskCache::minDurationMs() const {
  std::lock_guard<std::mutex> lock(mu_);
  return minDurationMs_;
}

std::filesystem::path DiskCache::fileOf(const std::string& cacheKey) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (root_.empty() || cacheKey.size() < 2) return {};
  const std::string name = safeName(cacheKey);
  return root_ / name.substr(0, 2) / (name + ".lfc");
}

bool DiskCache::has(const std::string& cacheKey, const std::vector<std::string>& ports) const {
  const std::filesystem::path file = fileOf(cacheKey);
  if (file.empty()) return false;
  std::string bytes;
  if (!readFile(file, bytes)) return false;
  Reader r{bytes};
  std::vector<PortEntry> table;
  return readTable(r, table) && covers(table, ports);
}

bool DiskCache::load(const std::string& cacheKey, const std::vector<std::string>& ports,
                     PortData& out) const {
  const std::filesystem::path file = fileOf(cacheKey);
  if (file.empty()) return false;
  std::string bytes;
  if (!readFile(file, bytes)) return false;
  PortData got;
  if (!decodeNode(bytes, got)) {
    std::error_code ec;
    std::filesystem::remove(file, ec);  // 坏文件（写到一半被杀、磁盘出错）：当未命中，删掉免得每次都读
    return false;
  }
  for (const auto& p : ports) {
    bool found = false;
    for (const auto& g : got) found = found || g.first == p;
    if (!found) return false;
  }
  out = std::move(got);
  return true;
}

void DiskCache::store(const std::string& cacheKey, const PortData& ports) const {
  const std::filesystem::path file = fileOf(cacheKey);
  if (file.empty()) return;
  std::string bytes;
  if (!encodeNode(ports, bytes)) return;
  static std::atomic<std::uint64_t> seq{0};
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp =
      file.parent_path() /
      (file.filename().u8string() + ".tmp" + std::to_string(LYFLOW_GETPID()) + "_" +
       std::to_string(seq.fetch_add(1)));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      return;
    }
  }
  std::filesystem::rename(tmp, file, ec);  // 同名文件已在（别的进程刚写完同一个节点）：内容相同，顶掉无妨
  if (ec) std::filesystem::remove(tmp, ec);
}

}  // namespace lyflow::exec
