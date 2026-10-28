#pragma once

#include "blender/BlenderTypes.h"

#include <functional>

namespace blendnet::blender {
inline constexpr std::string_view releaseRepository = "https://download.blender.org/release/";
using FetchListing = std::function<BlenderResult<std::string>(const std::string&,
                                                              const std::atomic_bool*)>;
BlenderResult<std::string> fetchOfficialListing(const std::string& url,
                                                const std::atomic_bool* cancelled);
// Stable releases with both an installable archive and its published SHA-256 file.
// Numeric descending order; a failed listing never produces an incomplete catalogue.
BlenderResult<std::vector<std::string>>
availableReleases(Platform platform, const std::atomic_bool* cancelled = nullptr,
                  FetchListing fetch = fetchOfficialListing);
} // namespace blendnet::blender
