#pragma once

#include "blender/BlenderTypes.h"

namespace blendnet::blender {
// Seam de transport pour les tests ; les URL restent construites par officialRelease.
using Download = std::function<BlenderError(const std::string&, const std::filesystem::path&,
                                            std::uint64_t, const std::atomic_bool*)>;
BlenderError downloadOfficial(const std::string& url, const std::filesystem::path& target,
                              std::uint64_t limit, const std::atomic_bool* cancelled);
class BlenderInstaller {
  public:
    explicit BlenderInstaller(std::filesystem::path cache, Download download = downloadOfficial);
    BlenderResult<VerifiedBlender> ensureVersion(std::string_view version,
                                                 const std::atomic_bool* cancelled = nullptr);

  private:
    std::filesystem::path m_cache;
    Download m_download;
};
} // namespace blendnet::blender
