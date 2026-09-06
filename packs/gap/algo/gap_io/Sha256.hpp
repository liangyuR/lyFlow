#pragma once

#include <filesystem>
#include <string>

namespace gap::io {

std::string sha256(const std::string& bytes);
std::string sha256File(const std::filesystem::path& path);

}  // namespace gap::io
