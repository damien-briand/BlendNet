#include "blender/BlenderTypes.h"

#include <charconv>
#include <sstream>

namespace blendnet::blender {
std::string_view toString(BlenderError error) {
    switch (error) {
    case BlenderError::None:
        return "ok";
    case BlenderError::InvalidRequest:
        return "invalid Blender request";
    case BlenderError::InvalidVersion:
        return "invalid Blender version";
    case BlenderError::UnsupportedPlatform:
        return "unsupported Blender platform";
    case BlenderError::DownloadFailed:
        return "official Blender download failed";
    case BlenderError::ChecksumMismatch:
        return "Blender archive SHA-256 mismatch; execution refused";
    case BlenderError::ArchiveRejected:
        return "Blender archive rejected";
    case BlenderError::CacheCorrupt:
        return "Blender cache integrity check failed; execution refused";
    case BlenderError::Busy:
        return "Blender installation already in progress";
    case BlenderError::IoError:
        return "Blender disk I/O failed";
    case BlenderError::ProcessFailed:
        return "Blender process failed";
    case BlenderError::Timeout:
        return "Blender operation timed out";
    case BlenderError::Cancelled:
        return "Blender operation cancelled";
    case BlenderError::UnsupportedBackend:
        return "selected Blender backend is not available";
    case BlenderError::MissingFrames:
        return "Blender did not produce all expected valid frames";
    }
    return "unknown Blender error";
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::optional<Platform> Platform::current() {
#if defined(_WIN32)
    constexpr auto system = System::Windows;
#elif defined(__APPLE__)
    constexpr auto system = System::MacOS;
#elif defined(__linux__)
    constexpr auto system = System::Linux;
#else
    return {};
#endif
#if defined(__x86_64__) || defined(_M_X64)
    return Platform{system, Architecture::X64};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return Platform{system, Architecture::Arm64};
#else
    return {};
#endif
}
bool officialDownloadUrl(std::string_view url) {
    constexpr std::string_view prefix = "https://download.blender.org/release/Blender";
    if (!url.starts_with(prefix) || url.size() > 512)
        return false;
    for (const auto c : url.substr(prefix.size()))
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '/' || c == '_'))
            return false;
    return url.find("..") == std::string_view::npos;
}
BlenderResult<Release> officialRelease(std::string_view version, Platform platform) {
    int components[3]{};
    std::size_t position = 0;
    for (int index = 0; index < 3; ++index) {
        const auto end = index == 2 ? version.size() : version.find('.', position);
        if (end == std::string_view::npos || end == position || end - position > 3)
            return {{}, BlenderError::InvalidVersion};
        const auto parsed =
            std::from_chars(version.data() + position, version.data() + end, components[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != version.data() + end ||
            components[index] < 0 || (end - position > 1 && version[position] == '0'))
            return {{}, BlenderError::InvalidVersion};
        position = end + 1;
    }
    if (components[0] < 3 || components[0] > 99)
        return {{}, BlenderError::InvalidVersion};
    const auto arch = platform.architecture == Architecture::X64 ? "x64" : "arm64";
    const auto os = platform.system == System::Linux     ? "linux"
                    : platform.system == System::Windows ? "windows"
                                                         : "macos";
    const auto extension = platform.system == System::Linux     ? ".tar.xz"
                           : platform.system == System::Windows ? ".zip"
                                                                : ".dmg";
    Release release;
    release.version = version;
    release.key = "blender-" + release.version + "-" + os + "-" + arch;
    release.topDirectory = platform.system == System::MacOS ? "Blender.app" : release.key;
    release.archiveName = release.key + extension;
    const auto base = "https://download.blender.org/release/Blender" +
                      std::to_string(components[0]) + "." + std::to_string(components[1]) + "/";
    release.archiveUrl = base + release.archiveName;
    release.checksumUrl = base + "blender-" + release.version + ".sha256";
    release.executable = std::filesystem::path(release.topDirectory) /
                         (platform.system == System::Linux     ? "blender"
                          : platform.system == System::Windows ? "blender.exe"
                                                               : "Contents/MacOS/Blender");
    return {release, BlenderError::None};
}
std::optional<std::string> publishedChecksum(std::string_view document, std::string_view filename) {
    if (document.size() > 65536)
        return {};
    std::istringstream lines{std::string(document)};
    std::string line;
    std::optional<std::string> result;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream fields(line);
        std::string hash, name, extra;
        if (!(fields >> hash >> name))
            continue;
        if (name.starts_with('*'))
            name.erase(0, 1);
        if (name != filename)
            continue;
        if (result || (fields >> extra) || hash.size() != 64)
            return {};
        for (auto& c : hash) {
            if (c >= 'A' && c <= 'F')
                c = char(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return {};
        }
        result = hash;
    }
    return result;
}
} // namespace blendnet::blender
