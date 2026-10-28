#pragma once

#include "jobs/Job.h"

#include <atomic>
#include <functional>
#include <memory>

namespace blendnet::blender {
enum class BlenderError {
    None,
    InvalidRequest,
    InvalidVersion,
    UnsupportedPlatform,
    DownloadFailed,
    ChecksumMismatch,
    ArchiveRejected,
    CacheCorrupt,
    Busy,
    IoError,
    ProcessFailed,
    Timeout,
    Cancelled,
    UnsupportedBackend,
    MissingFrames
};
std::string_view toString(BlenderError error);
template <typename T> struct BlenderResult {
    std::optional<T> value;
    BlenderError error = BlenderError::None;
    std::string diagnostic{};
    explicit operator bool() const { return value.has_value(); }
};
enum class System { Linux, Windows, MacOS };
enum class Architecture { X64, Arm64 };
struct Platform {
    System system;
    Architecture architecture;
    static std::optional<Platform> current();
};
struct Release {
    std::string version, key, archiveName, archiveUrl, checksumUrl, topDirectory;
    std::filesystem::path executable;
};
BlenderResult<Release> officialRelease(std::string_view version, Platform platform);
std::optional<std::string> publishedChecksum(std::string_view document, std::string_view filename);
bool officialDownloadUrl(std::string_view url);
std::string utf8(const std::filesystem::path& path);

class BlenderInstaller;
// Issued after official archive verification, or explicit local selection and version checking.
class VerifiedBlender {
  public:
    const std::filesystem::path& executable() const { return m_executable; }
    const std::filesystem::path& directory() const { return m_directory; }
    const std::string& version() const { return m_version; }
    std::filesystem::path runtimeDirectory() const {
        return m_runtime.empty() ? m_directory.parent_path() / "runtime" : m_runtime;
    }
    BlenderError validate(const std::atomic_bool* cancelled = nullptr) const;

  private:
    friend class BlenderInstaller;
    VerifiedBlender(std::filesystem::path directory, std::filesystem::path executable,
                    std::string version, std::string digest,
                    std::filesystem::path runtime = {}, bool local = false)
        : m_directory(std::move(directory)), m_executable(std::move(executable)),
          m_runtime(std::move(runtime)), m_version(std::move(version)),
          m_digest(std::move(digest)), m_local(local) {}
    std::filesystem::path m_directory, m_executable, m_runtime;
    std::string m_version, m_digest;
    bool m_local = false;
};
struct RenderRequest {
    protocol::TaskAssign task;
    std::filesystem::path scene, outputDirectory;
    std::string engine = "CYCLES", backend = "CPU";
    unsigned int threads = 2;
    std::chrono::milliseconds timeout{3600000};
};
struct RenderResult {
    std::vector<jobs::ValidatedFrame> frames;
    std::chrono::milliseconds elapsed{0};
};
} // namespace blendnet::blender
