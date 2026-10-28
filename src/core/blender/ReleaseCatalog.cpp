#include "blender/ReleaseCatalog.h"

#include <array>
#include <charconv>
#include <map>
#include <regex>
#include <set>

namespace blendnet::blender {
namespace {
std::set<std::string> links(const std::string& html) {
    static const std::regex anchor(R"(<a\s+href\s*=\s*["']([^"']+)["'])",
                                    std::regex::icase);
    std::set<std::string> result;
    for (auto it = std::sregex_iterator(html.begin(), html.end(), anchor);
         it != std::sregex_iterator{}; ++it)
        result.insert((*it)[1].str());
    return result;
}
std::array<int, 3> versionKey(const std::string& version) {
    std::array<int, 3> key{};
    const auto* position = version.data();
    for (int i = 0; i < 3; ++i) {
        const auto parsed = std::from_chars(position, version.data() + version.size(), key[i]);
        if (i < 2)
            position = parsed.ptr + 1;
    }
    return key;
}
} // namespace
BlenderResult<std::vector<std::string>> availableReleases(Platform platform,
                                                           const std::atomic_bool* cancelled,
                                                           FetchListing fetch) {
    try {
        if (cancelled && cancelled->load())
            return {{}, BlenderError::Cancelled};
        auto root = fetch(std::string(releaseRepository), cancelled);
        if (!root)
            return {{}, root.error};
        if (root.value->size() > 1024 * 1024)
            return {{}, BlenderError::DownloadFailed};
        static const std::regex directory(R"(^Blender([0-9]{1,2})\.([0-9]{1,3})/$)");
        std::set<std::pair<int, int>, std::greater<>> series;
        for (const auto& name : links(*root.value)) {
            std::smatch match;
            if (!std::regex_match(name, match, directory))
                continue;
            const int major = std::stoi(match[1]), minor = std::stoi(match[2]);
            if (major < 3 || name != "Blender" + std::to_string(major) + "." +
                                         std::to_string(minor) + "/")
                continue;
            series.emplace(major, minor);
        }
        if (series.empty() || series.size() > 128)
            return {{}, BlenderError::DownloadFailed};
        std::map<std::array<int, 3>, std::string, std::greater<>> versions;
        for (const auto& [major, minor] : series) {
            if (cancelled && cancelled->load())
                return {{}, BlenderError::Cancelled};
            const auto base = std::string(releaseRepository) + "Blender" +
                              std::to_string(major) + "." + std::to_string(minor) + "/";
            auto listing = fetch(base, cancelled);
            if (!listing)
                return {{}, listing.error};
            if (listing.value->size() > 1024 * 1024)
                return {{}, BlenderError::DownloadFailed};
            const auto files = links(*listing.value);
            for (const auto& name : files) {
                if (!name.starts_with("blender-") || !name.ends_with(".sha256"))
                    continue;
                const auto version = name.substr(8, name.size() - 8 - 7);
                const auto release = officialRelease(version, platform);
                if (!release || release.value->checksumUrl != base + name ||
                    !files.contains(release.value->archiveName))
                    continue;
                versions.emplace(versionKey(version), version);
                if (versions.size() > 4096)
                    return {{}, BlenderError::DownloadFailed};
            }
        }
        if (cancelled && cancelled->load())
            return {{}, BlenderError::Cancelled};
        if (versions.empty())
            return {{}, BlenderError::DownloadFailed};
        std::vector<std::string> result;
        for (const auto& [key, version] : versions)
            result.push_back(version);
        return {std::move(result), BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::DownloadFailed};
    }
}
} // namespace blendnet::blender
