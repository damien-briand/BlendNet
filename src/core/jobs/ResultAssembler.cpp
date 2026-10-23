#include "jobs/ResultAssembler.h"
#include "config/Utf8Path.h"
#include "transfer/TransferEngine.h"

#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>

namespace blendnet::jobs {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
bool stopped(const std::atomic_bool* flag) { return flag && flag->load(); }
using Archive = std::unique_ptr<zip_t, decltype(&zip_discard)>;
struct Staging {
    std::filesystem::path path;
    bool keep = false;
    ~Staging() {
        if (!keep) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};
std::uint32_t big(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}
std::uint32_t little(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
bool pngCrc(std::ifstream& input, const std::atomic_bool* cancelled = nullptr) {
    input.clear();
    input.seekg(8);
    std::array<unsigned char, 65536> buffer{};
    bool pixels = false;
    while (input) {
        if (stopped(cancelled))
            return false;
        std::array<unsigned char, 8> header{};
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        if (!input)
            return false;
        auto left = big(header.data());
        if (left > 512u * 1024 * 1024)
            return false;
        auto crc = crc32(0, header.data() + 4, 4);
        const bool end = std::memcmp(header.data() + 4, "IEND", 4) == 0;
        if (std::memcmp(header.data() + 4, "IDAT", 4) == 0)
            pixels = true;
        if (end && left != 0)
            return false;
        while (left) {
            if (stopped(cancelled))
                return false;
            const auto count = std::min<std::uint32_t>(left, buffer.size());
            input.read(reinterpret_cast<char*>(buffer.data()), count);
            if (!input)
                return false;
            crc = crc32(crc, buffer.data(), count);
            left -= count;
        }
        std::array<unsigned char, 4> stored{};
        input.read(reinterpret_cast<char*>(stored.data()), stored.size());
        if (!input || big(stored.data()) != crc)
            return false;
        if (end)
            return pixels && input.peek() == std::char_traits<char>::eof();
    }
    return false;
}
std::pair<std::uint32_t, std::uint32_t> dimensions(const std::filesystem::path& path,
                                                   std::string_view format,
                                                   const std::atomic_bool* cancelled = nullptr) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path))
        return {};
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 65536> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    const auto size = std::size_t(input.gcount());
    if (format == "PNG" && size >= 45 && std::memcmp(bytes.data(), "\211PNG\r\n\032\n", 8) == 0 &&
        big(bytes.data() + 8) == 13 && std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) {
        input.clear();
        input.seekg(-12, std::ios::end);
        std::array<unsigned char, 12> tail{};
        input.read(reinterpret_cast<char*>(tail.data()), tail.size());
        if (!input || big(tail.data()) != 0 || std::memcmp(tail.data() + 4, "IEND", 4) ||
            !pngCrc(input, cancelled))
            return {};
        return {big(bytes.data() + 16), big(bytes.data() + 20)};
    }
    if (format == "JPEG" && size >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8) {
        for (std::size_t p = 2; p + 4 < size;) {
            if (bytes[p++] != 0xff)
                return {};
            while (p < size && bytes[p] == 0xff)
                ++p;
            if (p + 3 >= size)
                return {};
            const auto marker = bytes[p++];
            if (marker == 0xd9 || marker == 0xda)
                break;
            const std::size_t length = (bytes[p] << 8) | bytes[p + 1];
            if (length < 2 || length > size - p)
                return {};
            if (((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                 (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) &&
                length >= 8)
                return {std::uint32_t((bytes[p + 5] << 8) | bytes[p + 6]),
                        std::uint32_t((bytes[p + 3] << 8) | bytes[p + 4])};
            p += length;
        }
    }
    if (format == "EXR" && size > 8 && little(bytes.data()) == 20000630) {
        std::size_t p = 8;
        while (p < size && bytes[p]) {
            auto end = p;
            while (end < size && bytes[end])
                ++end;
            if (end == size)
                return {};
            const std::string name(reinterpret_cast<char*>(bytes.data() + p), end - p);
            p = end + 1;
            end = p;
            while (end < size && bytes[end])
                ++end;
            if (end == size)
                return {};
            const std::string type(reinterpret_cast<char*>(bytes.data() + p), end - p);
            p = end + 1;
            if (size - p < 4)
                return {};
            const auto length = little(bytes.data() + p);
            p += 4;
            if (length > size - p)
                return {};
            if (name == "dataWindow" && type == "box2i" && length == 16) {
                const auto w = std::int64_t(std::int32_t(little(bytes.data() + p + 8))) -
                               std::int32_t(little(bytes.data() + p)) + 1;
                const auto h = std::int64_t(std::int32_t(little(bytes.data() + p + 12))) -
                               std::int32_t(little(bytes.data() + p + 4)) + 1;
                if (w > 0 && h > 0 && w <= UINT32_MAX && h <= UINT32_MAX)
                    return {std::uint32_t(w), std::uint32_t(h)};
                return {};
            }
            p += length;
        }
    }
    return {};
}
} // namespace
std::vector<ValidatedFrame> ResultAssembler::validate(const protocol::TaskAssign& task,
                                                      const std::filesystem::path& archive,
                                                      const std::filesystem::path& staging,
                                                      const std::atomic_bool* cancelled) {
    try {
        const FrameRange range{task.frameStart, task.frameEnd, task.frameStep};
        if (!range.count() || range.count() > 100000 || stopped(cancelled) ||
            std::filesystem::exists(staging))
            return {};
        Archive zip(zip_open(utf8(archive).c_str(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr),
                    zip_discard);
        if (!zip || zip_get_num_entries(zip.get(), 0) != std::int64_t(range.count()))
            return {};
        std::map<std::string, int> expected;
        for (std::int64_t n = range.start; n <= range.end; n += range.step)
            expected.emplace(frameName(int(n), task.outputFormat), int(n));
        std::uint64_t total = 0;
        for (zip_uint64_t i = 0; i < range.count(); ++i) {
            zip_stat_t stat{};
            zip_uint8_t os = 0;
            zip_uint32_t attrs = 0;
            if (zip_stat_index(zip.get(), i, 0, &stat) < 0 || !stat.name ||
                !expected.contains(stat.name) || stat.size == 0 ||
                stat.size > 512ULL * 1024 * 1024 || stat.encryption_method != ZIP_EM_NONE ||
                zip_file_get_external_attributes(zip.get(), i, 0, &os, &attrs) < 0 ||
                (os == ZIP_OPSYS_UNIX && ((attrs >> 16) & 0170000) != 0 &&
                 ((attrs >> 16) & 0170000) != 0100000))
                return {};
            total += stat.size;
            if (total > 2ULL * 1024 * 1024 * 1024)
                return {};
        }
        std::filesystem::create_directories(staging.parent_path());
        if (std::filesystem::space(staging.parent_path()).available < total ||
            !std::filesystem::create_directory(staging))
            return {};
        Staging cleanup{staging};
        std::vector<ValidatedFrame> frames;
        std::uint32_t width = 0, height = 0;
        std::set<std::string> names;
        for (zip_uint64_t i = 0; i < range.count(); ++i) {
            if (stopped(cancelled))
                return {};
            zip_stat_t stat{};
            if (zip_stat_index(zip.get(), i, 0, &stat) < 0 || !names.insert(stat.name).second)
                return {};
            const auto path = staging / pathFromUtf8(stat.name);
            std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
                zip_fopen_index(zip.get(), i, 0), zip_fclose);
            if (!file)
                return {};
            std::ofstream output(path, std::ios::binary);
            std::array<char, 65536> buffer{};
            std::uint64_t written = 0;
            while (true) {
                if (stopped(cancelled))
                    return {};
                const auto count = zip_fread(file.get(), buffer.data(), buffer.size());
                if (count < 0)
                    return {};
                if (!count)
                    break;
                written += count;
                if (written > stat.size)
                    return {};
                output.write(buffer.data(), count);
                if (!output)
                    return {};
            }
            output.close();
            if (!output || written != stat.size)
                return {};
            const auto [w, h] = dimensions(path, task.outputFormat, cancelled);
            if (!width) {
                width = w;
                height = h;
            }
            if (!w || !h || w != width || h != height)
                return {};
            frames.push_back({expected.at(stat.name), path, w, h});
        }
        cleanup.keep = true;
        return frames;
    } catch (const std::exception&) {
        return {};
    }
}
bool ResultAssembler::assemble(const Job& job, const std::map<int, ValidatedFrame>& frames,
                               const std::atomic_bool* cancelled, bool allowMissing) {
    try {
        const FrameRange range{job.frameStart, job.frameEnd, job.frameStep};
        if ((!allowMissing && frames.size() != range.count()) || !range.count() ||
            stopped(cancelled))
            return false;
        std::filesystem::create_directories(job.outputDirectory);
        if (frames.empty())
            return allowMissing;
        const auto width = frames.begin()->second.width, height = frames.begin()->second.height;
        for (std::int64_t n = range.start; n <= range.end; n += range.step) {
            const auto found = frames.find(int(n));
            if (allowMissing && found == frames.end())
                continue;
            if (stopped(cancelled) || found == frames.end() ||
                dimensions(found->second.path, job.outputFormat, cancelled) !=
                    std::pair{width, height})
                return false;
            const auto target = job.outputDirectory / frameName(int(n), job.outputFormat);
            if (std::filesystem::is_symlink(target))
                return false;
            std::filesystem::copy_file(found->second.path, target,
                                       std::filesystem::copy_options::overwrite_existing);
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
bool ResultAssembler::simulatedResult(const protocol::TaskAssign& task,
                                      const std::filesystem::path& archive) {
    // PNG 1x1 complet ; le simulateur supporte PNG, les formats Blender restent en P6/P7.
    static constexpr unsigned char png[] = {
        137, 80,  78,  71,  13, 10, 26, 10,  0,   0,  0,   13,  73,  72,  68, 82, 0,
        0,   0,   1,   0,   0,  0,  1,  8,   4,   0,  0,   0,   181, 28,  12, 2,  0,
        0,   0,   11,  73,  68, 65, 84, 120, 218, 99, 252, 255, 31,  0,   3,  3,  2,
        0,   239, 162, 167, 91, 0,  0,  0,   0,   73, 69,  78,  68,  174, 66, 96, 130};
    try {
        if (task.outputFormat != "PNG" ||
            !FrameRange{task.frameStart, task.frameEnd, task.frameStep}.count())
            return false;
        std::filesystem::create_directories(archive.parent_path());
        Archive zip(zip_open(utf8(archive).c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr),
                    zip_discard);
        if (!zip)
            return false;
        for (std::int64_t n = task.frameStart; n <= task.frameEnd; n += task.frameStep) {
            auto* source = zip_source_buffer(zip.get(), png, sizeof(png), 0);
            if (!source)
                return false;
            if (zip_file_add(zip.get(), frameName(int(n), "PNG").c_str(), source,
                             ZIP_FL_ENC_UTF_8) < 0) {
                zip_source_free(source);
                return false;
            }
        }
        auto* handle = zip.release();
        if (zip_close(handle) < 0) {
            zip_discard(handle);
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace blendnet::jobs
