#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace blendnet {

inline std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// Win32's extended namespace supports long file names without machine-wide policy changes.
// Normalize before adding the prefix: that namespace does not resolve '/' or '..'.
inline std::filesystem::path extendedFilePath(const std::filesystem::path& path) {
#ifdef _WIN32
    if (path.empty() || path.native().starts_with(L"\\\\?\\"))
        return path;
    const auto absolute = std::filesystem::absolute(path).lexically_normal().make_preferred();
    const auto& name = absolute.native();
    if (name.starts_with(L"\\\\"))
        return std::filesystem::path(L"\\\\?\\UNC\\" + name.substr(2));
    return std::filesystem::path(L"\\\\?\\" + name);
#else
    return path;
#endif
}

} // namespace blendnet
