#pragma once

#include "log/Logger.h"

#include <span>
#include <string>

namespace blendnet {
// RNF-07/S2: replace atomically, escape multiline entries and redact known tokens.
LogError exportLog(const std::filesystem::path& path, std::span<const std::string> entries,
                   std::span<const std::string> secrets = {});
} // namespace blendnet
