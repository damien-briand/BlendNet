#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace blendnet {

inline std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace blendnet
