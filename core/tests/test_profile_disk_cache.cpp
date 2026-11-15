#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "exec/disk_cache.h"
#include "lyflow/profile.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace lyflow;
using namespace lyflow::exec;

PortData sharedOutputs() {
  auto cloud = std::make_shared<PointCloud>();
  cloud->xyz = {1, 0, 2, NAN, NAN, NAN, 4, 0, 5};
  cloud->intensity = {10, 0, 20};
  cloud->normals = {0, 0, 1, 0, 0, 0, 0, 0, 1};
  cloud->rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255};
  Profile profile;
  profile.cloud = cloud;
  profile.descriptor = {1, "sensor", "mm", "XZ", "right", "acquisition", "samples", 4};
  profile.validity = {1, 0, 1};
  profile.sourceIndices = {0, 2, 3};
  auto mirror = profile;
  mirror.validity[0] = 0;
  return {{"indices", Data::indices({cloud->id, {2, 0}})},
          {"profile", Data::profile(profile)},
          {"cloud", Data::cloud(cloud)},
          {"mirror", Data::profile(mirror)}};
}

void checkShared(const PortData& values, std::uint64_t oldId) {
  REQUIRE(values.size() == 4);
  const auto* profile = values[1].second.asProfile();
  const auto* cloud = values[2].second.asCloud();
  const auto* mirror = values[3].second.asProfile();
  const auto* indices = values[0].second.asIndices();
  REQUIRE(profile);
  REQUIRE(cloud);
  REQUIRE(mirror);
  REQUIRE(indices);
  CHECK(profile->cloud.get() == cloud);
  CHECK(mirror->cloud.get() == cloud);
  CHECK(cloud->id != oldId);
  CHECK(indices->sourceCloudId == cloud->id);
  CHECK(indices->values == std::vector<std::int32_t>{2, 0});
  CHECK(profile->validity == std::vector<std::uint8_t>{1, 0, 1});
  CHECK(mirror->validity == std::vector<std::uint8_t>{0, 0, 1});
  CHECK(profile->sourceIndices == std::vector<std::uint32_t>{0, 2, 3});
  CHECK(std::isnan(cloud->xyz[3]));
  CHECK(cloud->rgb[8] == 255);
  CHECK(cloud->intensity[2] == 20);
  CHECK(cloud->normals[8] == 1);
}

template <class T> void patch(std::string& bytes, std::size_t at, T value) {
  REQUIRE(at <= bytes.size());
  REQUIRE(sizeof(value) <= bytes.size() - at);
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

std::size_t payloadOffset(const std::string& bytes, const std::string& wanted) {
  std::size_t at = 12;
  while (at < bytes.size()) {
    std::uint16_t n = 0;
    std::memcpy(&n, bytes.data() + at, sizeof(n));
    at += sizeof(n);
    const auto name = bytes.substr(at, n);
    at += n + 1;
    std::uint64_t length = 0;
    std::memcpy(&length, bytes.data() + at, sizeof(length));
    at += sizeof(length);
    if (name == wanted)
      return at;
    at += static_cast<std::size_t>(length);
  }
  FAIL("Missing port payload");
  return 0;
}

std::filesystem::path childDirectory() {
#if defined(_WIN32)
  const auto* path = _wgetenv(L"LYFLOW_TEST_PROFILE_CACHE_CHILD");
#else
  const auto* path = std::getenv("LYFLOW_TEST_PROFILE_CACHE_CHILD");
#endif
  return path ? std::filesystem::path(path) : std::filesystem::path{};
}

int runCacheChild(const std::filesystem::path& directory) {
#if defined(_WIN32)
  std::vector<wchar_t> executable(32768);
  const auto size =
      GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  REQUIRE(size > 0);
  REQUIRE(size < executable.size());
  const auto previous = childDirectory().wstring();
  REQUIRE(_wputenv_s(L"LYFLOW_TEST_PROFILE_CACHE_CHILD", directory.c_str()) == 0);
  const auto quoted = L"\"" + std::wstring(executable.data()) + L"\"";
  const wchar_t* args[]{quoted.c_str(), L"--test-case=*carrier*cache*", L"--no-colors", nullptr};
  const auto status = _wspawnv(_P_WAIT, executable.data(), args);
  REQUIRE(_wputenv_s(L"LYFLOW_TEST_PROFILE_CACHE_CHILD", previous.c_str()) == 0);
  return static_cast<int>(status);
#else
  const auto executable = std::filesystem::read_symlink("/proc/self/exe");
  const auto previous = childDirectory().string();
  REQUIRE(setenv("LYFLOW_TEST_PROFILE_CACHE_CHILD", directory.c_str(), 1) == 0);
  const auto child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    execl(executable.c_str(), executable.c_str(), "--test-case=*carrier*cache*", "--no-colors",
          static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  if (previous.empty())
    unsetenv("LYFLOW_TEST_PROFILE_CACHE_CHILD");
  else
    setenv("LYFLOW_TEST_PROFILE_CACHE_CHILD", previous.c_str(), 1);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("lyflow-profile-disk-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { std::filesystem::create_directories(path); }
  ~Directory() {
    DiskCache::instance().setDir("", "");
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
}

TEST_CASE("Profile disk carrier table retains cloud aliases and forward Indices references") {
  const auto original = sharedOutputs();
  const auto id = original[2].second.asCloud()->id;
  std::string bytes;
  REQUIRE(encodeNode(original, bytes));
  PortData decoded;
  REQUIRE(decodeNode(bytes, decoded));
  checkShared(decoded, id);
  CHECK(original[1].second.asProfile()->validity[0] == 1);
  CHECK(original[2].second.asCloud()->id == id);

  auto repeated = decoded;
  REQUIRE(decodeNode(bytes, repeated));
  checkShared(repeated, id);
  CHECK(repeated[2].second.asCloud()->id != decoded[2].second.asCloud()->id);
}

TEST_CASE("Profile disk carrier cache persists and reloads shared identities from a real file") {
  auto& cache = DiskCache::instance();
  const auto child = childDirectory();
  if (!child.empty()) {
    REQUIRE(cache.setDir(child.u8string(), "profile-codec-v2").empty());
    PortData restored;
    REQUIRE(cache.load("abcdef", {"indices", "profile", "cloud", "mirror"}, restored));
    checkShared(restored, 0);
    cache.setDir("", "");
    return;
  }
  Directory directory;
  REQUIRE(cache.setDir(directory.path.u8string(), "profile-codec-v2").empty());
  const auto original = sharedOutputs();
  cache.store("abcdef", original);
  const auto file = directory.path / "profile-codec-v2" / "ab" / "abcdef.lfc";
  REQUIRE(std::filesystem::exists(file));
  CHECK(runCacheChild(directory.path) == 0);
  CHECK(cache.has("abcdef", {"indices", "profile", "cloud", "mirror"}));
  PortData restored;
  REQUIRE(cache.load("abcdef", {"indices", "profile", "cloud", "mirror"}, restored));
  checkShared(restored, original[2].second.asCloud()->id);

  std::ifstream input(file, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  input.close();
  patch(bytes, 4, std::uint32_t{1});
  std::ofstream old(file, std::ios::binary | std::ios::trunc);
  old.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  old.close();
  CHECK_FALSE(cache.has("abcdef", {"profile"}));
  CHECK_FALSE(cache.load("abcdef", {"profile"}, restored));
  CHECK_FALSE(std::filesystem::exists(file));
  cache.store("abcdef", original);
  REQUIRE(cache.load("abcdef", {"profile"}, restored));
  checkShared(restored, original[2].second.asCloud()->id);
}

TEST_CASE("Profile disk carrier corruption and unresolved references fail atomically") {
  const auto original = sharedOutputs();
  std::string bytes;
  REQUIRE(encodeNode(original, bytes));
  PortData restored;
  REQUIRE(decodeNode(bytes, restored));
  const auto keep = restored[1].second.profilePtr();
  const auto cloud = payloadOffset(bytes, "cloud");
  const auto indices = payloadOffset(bytes, "indices");
  for (const auto* defect : {"old-version", "unknown-alias", "invalid-alias", "unknown-indices",
                             "negative-index", "large-index", "truncated"}) {
    CAPTURE(defect);
    auto corrupt = bytes;
    const std::string kind(defect);
    if (kind == "old-version")
      patch(corrupt, 4, std::uint32_t{1});
    if (kind == "unknown-alias")
      patch(corrupt, cloud, std::uint64_t{0});
    if (kind == "invalid-alias")
      patch(corrupt, cloud + 8, std::uint8_t{2});
    if (kind == "unknown-indices")
      patch(corrupt, indices, std::uint64_t{0});
    if (kind == "negative-index")
      patch(corrupt, indices + 16, std::int32_t{-1});
    if (kind == "large-index")
      patch(corrupt, indices + 16, std::int32_t{3});
    if (kind == "truncated")
      corrupt.pop_back();
    CHECK_FALSE(decodeNode(corrupt, restored));
    CHECK(restored[1].second.profilePtr() == keep);
  }
  auto conflicting = original;
  PointCloud changed = *original[2].second.asCloud();
  changed.xyz[0] += 1;
  conflicting[2].second = Data::cloud(std::move(changed));
  std::string untouched = "unchanged";
  CHECK_FALSE(encodeNode(conflicting, untouched));
  CHECK(untouched == "unchanged");
  conflicting = original;
  conflicting[0].second = Data::indices({0, {0}});
  CHECK_FALSE(encodeNode(conflicting, untouched));
  conflicting[0].second = Data::indices({original[2].second.asCloud()->id, {3}});
  CHECK_FALSE(encodeNode(conflicting, untouched));
  CHECK(untouched == "unchanged");
}
