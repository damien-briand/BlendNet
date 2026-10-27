#include "log/LogExporter.h"

#include <atomic>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace blendnet {
LogError exportLog(const std::filesystem::path& path, std::span<const std::string> entries,
                   std::span<const std::string> secrets) {
    std::filesystem::path temporary;
    struct Cleanup {
        std::filesystem::path& path;
        ~Cleanup() {
            std::error_code error;
            if (!path.empty())
                std::filesystem::remove(path, error);
        }
    } cleanup{temporary};
    try {
        if (path.empty() || std::filesystem::is_symlink(path) ||
            (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path)))
            return LogError::OpenFailed;
        static std::atomic<unsigned long long> sequence{0};
        temporary = path;
        temporary +=
            "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "." + std::to_string(sequence++) + ".tmp";
        if (std::filesystem::exists(temporary) || std::filesystem::is_symlink(temporary)) {
            temporary.clear();
            return LogError::OpenFailed;
        }
        std::ofstream output(temporary, std::ios::binary);
        if (!output)
            return LogError::OpenFailed;
#ifndef _WIN32
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read |
                                                    std::filesystem::perms::owner_write);
#endif
        for (auto entry : entries) {
            for (const auto& secret : secrets) {
                if (secret.empty())
                    continue;
                std::size_t offset = 0;
                while ((offset = entry.find(secret, offset)) != std::string::npos) {
                    entry.replace(offset, secret.size(), "[redacted]");
                    offset += 10;
                }
            }
            for (const char c : entry) {
                if (c == '\n')
                    output << "\\n";
                else if (c == '\r')
                    output << "\\r";
                else if (c == '\\')
                    output << "\\\\";
                else
                    output << c;
            }
            output << '\n';
        }
        output.close();
        if (!output)
            return LogError::WriteFailed;
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return LogError::WriteFailed;
#else
        std::filesystem::rename(temporary, path);
#endif
        return LogError::None;
    } catch (const std::exception&) {
        return LogError::WriteFailed;
    }
}
} // namespace blendnet
