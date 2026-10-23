#pragma once

#include "jobs/Job.h"

#include <atomic>

namespace blendnet::archive {
struct ZipLimits {
    std::uint64_t expandedBytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t fileBytes = 2ULL * 1024 * 1024 * 1024;
    std::uint64_t entries = 100000;
};
// All disk work runs on the caller's worker. Destinations must be fresh, private directories.
class ZipService {
  public:
    static bool privateDirectory(const std::filesystem::path& path);
    // Optional blendnet-project.json selects blendPath; otherwise exactly one .blend is required.
    static std::optional<std::filesystem::path>
    extractProject(const std::filesystem::path& archive, const std::filesystem::path& destination,
                   ZipLimits limits = {}, const std::atomic_bool* cancelled = nullptr);
    // Only inspected frames in the assigned range are included. Partial results carry a report.
    static bool packResult(const protocol::TaskAssign& task, const std::filesystem::path& frames,
                           const std::filesystem::path& destination, std::string_view error = {},
                           const std::atomic_bool* cancelled = nullptr);
};
} // namespace blendnet::archive
