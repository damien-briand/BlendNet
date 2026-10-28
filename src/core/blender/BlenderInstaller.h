#pragma once

#include "blender/BlenderTypes.h"

namespace blendnet::blender {
// Seam de transport pour les tests ; les URL restent construites par officialRelease.
using Download = std::function<BlenderError(const std::string&, const std::filesystem::path&,
                                            std::uint64_t, const std::atomic_bool*)>;
using BlenderLog = std::function<void(std::string_view)>;
BlenderError downloadOfficial(const std::string& url, const std::filesystem::path& target,
                              std::uint64_t limit, const std::atomic_bool* cancelled);
BlenderError downloadOfficialDiagnosed(const std::string& url, const std::filesystem::path& target,
                                       std::uint64_t limit, const std::atomic_bool* cancelled,
                                       std::string& diagnostic);
class BlenderInstaller {
  public:
    explicit BlenderInstaller(std::filesystem::path cache, Download download = {},
                               std::filesystem::path localExecutable = {}, BlenderLog log = {});
    BlenderResult<VerifiedBlender> ensureVersion(std::string_view version,
                                                 const std::atomic_bool* cancelled = nullptr);

  private:
    std::filesystem::path m_cache;
    Download m_download;
    std::filesystem::path m_localExecutable;
    BlenderLog m_log;
};
} // namespace blendnet::blender
