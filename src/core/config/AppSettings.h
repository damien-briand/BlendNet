#pragma once

#include "log/Logger.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blendnet {

// RF-10 / RNF-S3 : le pin est persisté après HELLO_OK lors de l'appairage.
struct KnownSource {
    std::string host;
    int port = 47050;
    std::string token;
    std::optional<std::string> certificateSha256;

    bool operator==(const KnownSource&) const = default;
};

enum class SettingsError {
    None,
    NotFound,
    ReadFailed,
    InvalidJson,
    InvalidValue,
    UnsupportedVersion,
    WriteFailed
};

std::string_view toString(SettingsError error);

// Socle RF-10, RF-12, RF-24, RF-25 et RNF-07. Aucun type JSON dans l'API publique.
struct AppSettings {
    static constexpr int schemaVersion = 1;

    std::string language = "fr";
    LogLevel logLevel = LogLevel::Info;
    std::string computeBackend = "CPU";
    std::filesystem::path workDirectory = "jobs";
    std::filesystem::path blenderCacheDirectory = "blender-cache";
    std::optional<int> benchmarkScore;
    bool keepCompletedJobs = false;
    std::vector<KnownSource> knownSources;

    bool operator==(const AppSettings&) const = default;

    static std::filesystem::path defaultPath();
    // En cas d'erreur, la destination et le fichier existant restent inchangés.
    static SettingsError load(const std::filesystem::path& path, AppSettings& destination);
    SettingsError save(const std::filesystem::path& path) const;
    SettingsError validate() const;
};

} // namespace blendnet
