#include "archive/ZipService.h"
#include "config/Utf8Path.h"
#include "jobs/ResultAssembler.h"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <set>

namespace blendnet::archive {
namespace {
using Archive = std::unique_ptr<zip_t, decltype(&zip_discard)>;
bool stopped(const std::atomic_bool* flag) { return flag && flag->load(); }
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
std::string folded(std::string text) {
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return text;
}
bool safeName(std::string_view name) {
    if (name.empty() || name.size() > 1024 || name.front() == '/' || name.back() == '/')
        return false;
    unsigned int depth = 0;
    for (std::size_t start = 0; start < name.size();) {
        auto end = name.find('/', start);
        if (end == name.npos)
            end = name.size();
        const auto part = name.substr(start, end - start);
        if (++depth > 32 || part.empty() || part.size() > 255 || part == "." || part == ".." ||
            part.back() == '.' || part.back() == ' ')
            return false;
        for (const auto c : part)
            if (static_cast<unsigned char>(c) < 32 || c == 127 ||
                std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos)
                return false;
        const auto stem = folded(std::string(part.substr(0, part.find('.'))));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
             stem[3] >= '0' && stem[3] <= '9'))
            return false;
        start = end + 1;
    }
    return true;
}
struct Cleanup {
    std::filesystem::path path;
    bool keep = false;
    ~Cleanup() {
        if (!keep) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};
} // namespace
bool ZipService::privateDirectory(const std::filesystem::path& path) {
    try {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        auto current = absolute.root_path();
        for (const auto& part : absolute.relative_path()) {
            current /= part;
            if (std::filesystem::is_symlink(current))
                return false;
            std::filesystem::create_directory(current);
            if (!std::filesystem::is_directory(current))
                return false;
        }
        std::filesystem::permissions(absolute, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
std::optional<std::filesystem::path>
ZipService::extractProject(const std::filesystem::path& archive,
                           const std::filesystem::path& destination, ZipLimits limits,
                           const std::atomic_bool* cancelled) {
    try {
        if (stopped(cancelled) || std::filesystem::exists(destination) ||
            std::filesystem::is_symlink(destination) ||
            !privateDirectory(destination.parent_path()))
            return {};
        Archive zip(zip_open(utf8(archive).c_str(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr),
                    zip_discard);
        if (!zip)
            return {};
        const auto count = zip_get_num_entries(zip.get(), 0);
        if (count <= 0 || std::uint64_t(count) > limits.entries)
            return {};
        struct Entry {
            std::string name;
            std::uint64_t size;
            bool directory;
        };
        std::vector<Entry> entries;
        std::map<std::string, bool> paths;
        std::map<std::string, std::string> portableNames;
        std::uint64_t total = 0;
        for (zip_uint64_t i = 0; i < std::uint64_t(count); ++i) {
            zip_stat_t stat{};
            zip_uint8_t os{};
            zip_uint32_t attrs{};
            if (stopped(cancelled) || zip_stat_index(zip.get(), i, ZIP_FL_ENC_STRICT, &stat) < 0 ||
                !stat.name || stat.encryption_method != ZIP_EM_NONE ||
                zip_file_get_external_attributes(zip.get(), i, 0, &os, &attrs) < 0)
                return {};
            std::string name = stat.name;
            const bool directory = name.ends_with('/');
            if (directory)
                name.pop_back();
            const auto type = (attrs >> 16) & 0170000;
            if (!safeName(name) ||
                (os == ZIP_OPSYS_UNIX && type != 0 && type != (directory ? 0040000u : 0100000u)) ||
                (attrs & 0x400) || (directory && stat.size != 0) || stat.size > limits.fileBytes ||
                stat.size > limits.expandedBytes - total ||
                !paths.emplace(folded(name), directory).second)
                return {};
            for (std::size_t end = 0; end != std::string::npos;) {
                end = name.find('/', end + 1);
                const auto prefix = name.substr(0, end);
                const auto [found, inserted] = portableNames.emplace(folded(prefix), prefix);
                if (!inserted && found->second != prefix)
                    return {};
            }
            total += stat.size;
            entries.push_back({std::move(name), stat.size, directory});
        }
        // Reject file/directory conflicts and portable case aliases before creating any file.
        for (const auto& [name, directory] : paths) {
            (void)directory;
            for (auto pos = name.find('/'); pos != name.npos; pos = name.find('/', pos + 1)) {
                const auto parent = paths.find(name.substr(0, pos));
                if (parent != paths.end() && !parent->second)
                    return {};
            }
        }
        if (std::filesystem::space(destination.parent_path()).available < total ||
            !std::filesystem::create_directory(destination))
            return {};
        Cleanup cleanup{destination};
        std::filesystem::permissions(destination, std::filesystem::perms::owner_all);
        std::vector<std::filesystem::path> scenes;
        for (zip_uint64_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            const auto target = destination / pathFromUtf8(entry.name);
            if (entry.directory) {
                if (!privateDirectory(target))
                    return {};
                continue;
            }
            if (!privateDirectory(target.parent_path()))
                return {};
            // Also catch aliases imposed by the host filesystem (Unicode normalization,
            // case-insensitive volumes). Never overwrite an earlier extracted entry.
            if (std::filesystem::exists(target) || std::filesystem::is_symlink(target))
                return {};
            std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
                zip_fopen_index(zip.get(), i, 0), zip_fclose);
            if (!file)
                return {};
            std::ofstream output(target, std::ios::binary);
            std::array<char, 65536> buffer{};
            std::uint64_t written = 0;
            while (true) {
                if (stopped(cancelled))
                    return {};
                const auto n = zip_fread(file.get(), buffer.data(), buffer.size());
                if (n < 0 || std::uint64_t(n) > entry.size - written)
                    return {};
                if (!n)
                    break;
                output.write(buffer.data(), n);
                if (!output)
                    return {};
                written += n;
            }
            output.close();
            if (!output || written != entry.size)
                return {};
            std::filesystem::permissions(target, std::filesystem::perms::owner_read |
                                                     std::filesystem::perms::owner_write);
            if (target.extension() == ".blend")
                scenes.push_back(target);
        }
        std::filesystem::path scene;
        const auto manifest = destination / "blendnet-project.json";
        if (std::filesystem::exists(manifest)) {
            if (!std::filesystem::is_regular_file(manifest) ||
                std::filesystem::file_size(manifest) > 65536)
                return {};
            std::ifstream input(manifest);
            const auto data = nlohmann::json::parse(input);
            const auto name = data.at("blendPath").get<std::string>();
            if (data.at("schemaVersion") != 1 || !safeName(name))
                return {};
            scene = destination / pathFromUtf8(name);
            if (std::find(scenes.begin(), scenes.end(), scene) == scenes.end())
                return {};
        } else {
            if (scenes.size() != 1)
                return {};
            scene = scenes.front();
        }
        cleanup.keep = true;
        return scene;
    } catch (const std::exception&) {
        return {};
    }
}
bool ZipService::packResult(const protocol::TaskAssign& task, const std::filesystem::path& frames,
                            const std::filesystem::path& destination, std::string_view error,
                            const std::atomic_bool* cancelled) {
    try {
        const jobs::FrameRange range{task.frameStart, task.frameEnd, task.frameStep};
        if (!range.count() || range.count() > 100000 || stopped(cancelled) ||
            std::filesystem::exists(destination) || std::filesystem::is_symlink(destination) ||
            !privateDirectory(destination.parent_path()))
            return false;
        Cleanup cleanup{destination};
        Archive zip(zip_open(utf8(destination).c_str(), ZIP_CREATE | ZIP_EXCL, nullptr),
                    zip_discard);
        if (!zip)
            return false;
        zip_register_cancel_callback_with_state(
            zip.get(),
            [](zip_t*, void* state) {
                return stopped(static_cast<const std::atomic_bool*>(state)) ? 1 : 0;
            },
            nullptr, const_cast<std::atomic_bool*>(cancelled));
        nlohmann::json missing = nlohmann::json::array();
        std::uint64_t bytes = 0;
        std::size_t valid = 0;
        std::uint32_t width = 0, height = 0;
        for (std::int64_t n = range.start; n <= range.end; n += range.step) {
            const auto name = jobs::frameName(int(n), task.outputFormat);
            const auto path = frames / name;
            const auto frame =
                jobs::ResultAssembler::inspect(int(n), path, task.outputFormat, cancelled);
            if (!frame || (width && (frame->width != width || frame->height != height))) {
                missing.push_back(int(n));
                continue;
            }
            width = frame->width;
            height = frame->height;
            const auto size = std::filesystem::file_size(path);
            if (size > 512ULL * 1024 * 1024 || size > 2ULL * 1024 * 1024 * 1024 - bytes)
                return false;
            bytes += size;
            auto* source = zip_source_file(zip.get(), utf8(path).c_str(), 0, ZIP_LENGTH_TO_END);
            if (!source)
                return false;
            if (zip_file_add(zip.get(), name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
                zip_source_free(source);
                return false;
            }
            ++valid;
        }
        if (!valid || stopped(cancelled) || (error.empty() && !missing.empty()))
            return false;
        std::string report;
        if (!error.empty() || !missing.empty()) {
            report = nlohmann::json{{"schemaVersion", 1},
                                    {"jobId", task.jobId},
                                    {"chunkId", task.chunkId},
                                    {"error", error},
                                    {"missingFrames", missing}}
                         .dump();
            auto* source = zip_source_buffer(zip.get(), report.data(), report.size(), 0);
            if (!source)
                return false;
            if (zip_file_add(zip.get(), "blendnet-errors.json", source, 0) < 0) {
                zip_source_free(source);
                return false;
            }
        }
        // report must outlive zip_close (sources are streamed, not buffered by BlendNet).
        if (zip_close(zip.get()) < 0)
            return false;
        zip.release();
        cleanup.keep = true;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace blendnet::archive
