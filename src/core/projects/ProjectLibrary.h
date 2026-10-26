#pragma once

#include "blender/BlenderTypes.h"

namespace blendnet::projects {
struct ProjectEntry {
    std::string name;
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uint64_t bytes = 0;
};
struct ProjectInfo {
    ProjectEntry entry;
    std::string engine, format, version;
    int start = 1, end = 1, step = 1, width = 0, height = 0;
    std::uint64_t estimatedBytes = 0;
    std::vector<std::filesystem::path> dependencies;
};
class ProjectLibrary {
  public:
    static std::vector<ProjectEntry> scan(const std::vector<std::filesystem::path>& directories,
                                          const std::vector<std::filesystem::path>& hidden = {},
                                          const std::atomic_bool* cancelled = nullptr);
    static blender::BlenderResult<ProjectInfo> inspect(const blender::VerifiedBlender& binary,
                                                       const std::filesystem::path& scene,
                                                       const std::atomic_bool* cancelled = nullptr);
    // Detected dependencies and explicitly selected cache directories are preserved.
    // External dependencies must first be collected in this directory, never silently omitted.
    static blender::BlenderError package(const blender::VerifiedBlender& binary,
                                         const ProjectInfo& project,
                                         const std::filesystem::path& archive,
                                         const std::atomic_bool* cancelled = nullptr);
};
} // namespace blendnet::projects
