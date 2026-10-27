#pragma once

#include <filesystem>

namespace blendnet {
// RNF-08/09: resources follow the executable when a package is relocated.
std::filesystem::path executableDirectory();
std::filesystem::path bundledFont();
} // namespace blendnet
