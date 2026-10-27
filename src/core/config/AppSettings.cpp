#include "config/AppSettings.h"
#include "config/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace blendnet {
namespace {

using Json = nlohmann::json;
std::recursive_mutex settingsFileMutex;

std::filesystem::path environmentPath(const char* name) {
    const auto* value = std::getenv(name);
    return value && *value ? pathFromUtf8(value) : std::filesystem::path{};
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

int integer(const Json& value, int minimum, int maximum) {
    if (!value.is_number_integer() || value < minimum || value > maximum) {
        throw std::invalid_argument("Invalid settings integer");
    }
    return value.get<int>();
}

bool isFingerprint(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

bool isUsablePath(const std::filesystem::path& path) {
    const auto& native = path.native();
    return !native.empty() && native.find(std::filesystem::path::value_type{}) == native.npos;
}

// Suppression du fichier temporaire, y compris après une écriture/renommage échoué.
struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

} // namespace

std::string_view toString(SettingsError error) {
    switch (error) {
    case SettingsError::None:
        return "ok";
    case SettingsError::NotFound:
        return "settings file not found";
    case SettingsError::ReadFailed:
        return "cannot read settings file";
    case SettingsError::InvalidJson:
        return "invalid settings JSON";
    case SettingsError::InvalidValue:
        return "invalid settings value";
    case SettingsError::UnsupportedVersion:
        return "unsupported settings version";
    case SettingsError::WriteFailed:
        return "cannot write settings file";
    }
    return "unknown settings error";
}

std::filesystem::path AppSettings::defaultPath() {
#ifdef _WIN32
    const auto base = environmentPath("APPDATA");
    if (!base.empty()) {
        return base / "BlendNet" / "settings.json";
    }
#elif defined(__APPLE__)
    const auto home = environmentPath("HOME");
    if (!home.empty()) {
        return home / "Library" / "Application Support" / "BlendNet" / "settings.json";
    }
#else
    const auto config = environmentPath("XDG_CONFIG_HOME");
    if (config.is_absolute()) {
        return config / "blendnet" / "settings.json";
    }
    const auto home = environmentPath("HOME");
    if (!home.empty()) {
        return home / ".config" / "blendnet" / "settings.json";
    }
#endif
    return std::filesystem::path(".blendnet") / "settings.json";
}

SettingsError AppSettings::validate() const {
    constexpr std::array backends{"OPTIX", "CUDA", "HIP", "ONEAPI", "METAL", "CPU"};
    constexpr std::array availabilityValues{"AVAILABLE", "BUSY_GAME", "BUSY_WORK", "DND",
                                            "OFFLINE"};
    if ((language != "fr" && language != "en") || logLevel < LogLevel::Debug ||
        logLevel > LogLevel::Off ||
        std::find(backends.begin(), backends.end(), computeBackend) == backends.end() ||
        !isUsablePath(workDirectory) || !isUsablePath(blenderCacheDirectory) ||
        (benchmarkScore && *benchmarkScore < 0) ||
        std::find(availabilityValues.begin(), availabilityValues.end(), availability) ==
            availabilityValues.end() ||
        maxFramesPerJob < 0 || quotaGB < 0 || renderThreads < 1 || renderThreads > 1024 ||
        renderTimeoutSec < 1 || renderTimeoutSec > 86400 || offerTimeoutSec < 1 ||
        offerTimeoutSec > 3600 || availableFromHour < 0 || availableFromHour > 23 ||
        availableUntilHour < 0 || availableUntilHour > 23 || serverPort < 1 || serverPort > 65535 ||
        blenderVersion.empty() || blenderVersion.size() > 32 || publicAddress.size() > 255 ||
        publicAddress.find_first_of("\r\n\t") != std::string::npos ||
        projectDirectories.size() > 128 || hiddenProjects.size() > 10000 ||
        knownSources.size() > 64) {
        return SettingsError::InvalidValue;
    }
    for (const auto& engine : excludedEngines)
        if (engine != "CYCLES" && engine != "EEVEE")
            return SettingsError::InvalidValue;
    for (const auto& paths : {projectDirectories, hiddenProjects})
        for (const auto& path : paths)
            if (!isUsablePath(path))
                return SettingsError::InvalidValue;
    for (const auto& source : knownSources) {
        if (source.host.empty() || source.host.find_first_of("\r\n\t ") != std::string::npos ||
            source.host.find('\0') != std::string::npos || source.port < 1 || source.port > 65535 ||
            source.token.empty() ||
            (source.certificateSha256 && !isFingerprint(*source.certificateSha256))) {
            return SettingsError::InvalidValue;
        }
    }
    return SettingsError::None;
}

SettingsError AppSettings::load(const std::filesystem::path& path, AppSettings& destination) {
    const std::lock_guard lock(settingsFileMutex);
    std::error_code error;
    // exists(path, error) efface aussi ENOTDIR en signalant simplement « absent ».
    // Garder l'erreur de status pour distinguer un parent fichier d'un chemin inexistant.
    const auto status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(status))) {
        // Windows can report PATH_NOT_FOUND even when an ancestor is a file.
        // Find the first existing parent before classifying the path as missing.
        for (auto parent = path.parent_path(); !parent.empty();) {
            std::error_code parentError;
            const auto parentStatus = std::filesystem::status(parent, parentError);
            if (parentError && parentError != std::errc::no_such_file_or_directory)
                return SettingsError::ReadFailed;
            if (std::filesystem::exists(parentStatus))
                return std::filesystem::is_directory(parentStatus) ? SettingsError::NotFound
                                                                  : SettingsError::ReadFailed;
            const auto next = parent.parent_path();
            if (next == parent)
                break;
            parent = next;
        }
        return SettingsError::NotFound;
    }
    if (error) {
        return SettingsError::ReadFailed;
    }
    if (!std::filesystem::exists(status)) {
        return SettingsError::NotFound;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return SettingsError::ReadFailed;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return SettingsError::ReadFailed;
    }

    try {
        const auto data = Json::parse(input, nullptr, false);
        if (input.bad()) {
            return SettingsError::ReadFailed;
        }
        if (data.is_discarded()) {
            return SettingsError::InvalidJson;
        }
        if (!data.is_object()) {
            return SettingsError::InvalidValue;
        }
        if (data.contains("schemaVersion") &&
            integer(data.at("schemaVersion"), 1, std::numeric_limits<int>::max()) !=
                schemaVersion) {
            return SettingsError::UnsupportedVersion;
        }

        AppSettings settings;
        settings.language = data.value("language", settings.language);
        settings.computeBackend = data.value("computeBackend", settings.computeBackend);
        settings.keepCompletedJobs = data.value("keepCompletedJobs", settings.keepCompletedJobs);
        settings.blenderVersion = data.value("blenderVersion", settings.blenderVersion);
        settings.availability = data.value("availability", settings.availability);
        settings.publicAddress = data.value("publicAddress", settings.publicAddress);
        settings.excludedEngines = data.value("excludedEngines", settings.excludedEngines);
        settings.scheduleEnabled = data.value("scheduleEnabled", settings.scheduleEnabled);
        settings.stopOnActivity = data.value("stopOnActivity", settings.stopOnActivity);
        settings.backgroundRendering =
            data.value("backgroundRendering", settings.backgroundRendering);
        settings.benchmarkPrompted = data.value("benchmarkPrompted", settings.benchmarkPrompted);
        const auto settingInt = [&](const char* key, int& value, int minimum, int maximum) {
            if (data.contains(key))
                value = integer(data.at(key), minimum, maximum);
        };
        settingInt("maxFramesPerJob", settings.maxFramesPerJob, 0, INT32_MAX);
        settingInt("quotaGB", settings.quotaGB, 0, INT32_MAX);
        settingInt("renderThreads", settings.renderThreads, 1, 1024);
        settingInt("renderTimeoutSec", settings.renderTimeoutSec, 1, 86400);
        settingInt("offerTimeoutSec", settings.offerTimeoutSec, 1, 3600);
        settingInt("availableFromHour", settings.availableFromHour, 0, 23);
        settingInt("availableUntilHour", settings.availableUntilHour, 0, 23);
        settingInt("serverPort", settings.serverPort, 1, 65535);
        const auto paths = [&](const char* key, std::vector<std::filesystem::path>& result) {
            if (data.contains(key)) {
                if (!data.at(key).is_array())
                    throw std::invalid_argument("Invalid paths");
                for (const auto& path : data.at(key))
                    result.push_back(pathFromUtf8(path.get<std::string>()));
            }
        };
        paths("projectDirectories", settings.projectDirectories);
        paths("hiddenProjects", settings.hiddenProjects);
        const auto level = parseLogLevel(data.value("logLevel", std::string("info")));
        if (!level) {
            return SettingsError::InvalidValue;
        }
        settings.logLevel = *level;
        settings.workDirectory =
            pathFromUtf8(data.value("workDirectory", pathUtf8(settings.workDirectory)));
        settings.blenderCacheDirectory = pathFromUtf8(
            data.value("blenderCacheDirectory", pathUtf8(settings.blenderCacheDirectory)));
        if (data.contains("benchmarkScore") && !data.at("benchmarkScore").is_null()) {
            settings.benchmarkScore =
                integer(data.at("benchmarkScore"), 0, std::numeric_limits<int>::max());
        }
        if (data.contains("knownSources")) {
            const auto& sources = data.at("knownSources");
            if (!sources.is_array()) {
                return SettingsError::InvalidValue;
            }
            for (const auto& entry : sources) {
                KnownSource source;
                source.host = entry.at("host").get<std::string>();
                source.port = integer(entry.at("port"), 1, 65535);
                source.token = entry.at("token").get<std::string>();
                if (entry.contains("certificateSha256") &&
                    !entry.at("certificateSha256").is_null()) {
                    source.certificateSha256 = entry.at("certificateSha256").get<std::string>();
                }
                settings.knownSources.push_back(std::move(source));
            }
        }
        if (settings.validate() != SettingsError::None) {
            return SettingsError::InvalidValue;
        }
        destination = std::move(settings);
        return SettingsError::None;
    } catch (const Json::exception&) {
        // Ne pas exposer le détail du parseur : il pourrait contenir un token (RNF-S2).
        return SettingsError::InvalidValue;
    } catch (const std::invalid_argument&) {
        return SettingsError::InvalidValue;
    } catch (const std::filesystem::filesystem_error&) {
        return SettingsError::InvalidValue;
    } catch (const std::ios_base::failure&) {
        return SettingsError::ReadFailed;
    }
}

SettingsError AppSettings::save(const std::filesystem::path& path) const {
    const std::lock_guard lock(settingsFileMutex);
    if (validate() != SettingsError::None) {
        return SettingsError::InvalidValue;
    }
    try {
        Json sources = Json::array();
        for (const auto& source : knownSources) {
            sources.push_back(
                {{"host", source.host},
                 {"port", source.port},
                 {"token", source.token},
                 {"certificateSha256",
                  source.certificateSha256 ? Json(*source.certificateSha256) : Json(nullptr)}});
        }
        Json data{{"schemaVersion", schemaVersion},
                  {"language", language},
                  {"logLevel", toString(logLevel)},
                  {"computeBackend", computeBackend},
                  {"keepCompletedJobs", keepCompletedJobs},
                  {"workDirectory", pathUtf8(workDirectory)},
                  {"blenderCacheDirectory", pathUtf8(blenderCacheDirectory)},
                  {"benchmarkScore", benchmarkScore ? Json(*benchmarkScore) : Json(nullptr)},
                  {"knownSources", std::move(sources)}};
        data.update({{"blenderVersion", blenderVersion},
                     {"availability", availability},
                     {"excludedEngines", excludedEngines},
                     {"maxFramesPerJob", maxFramesPerJob},
                     {"quotaGB", quotaGB},
                     {"renderThreads", renderThreads},
                     {"renderTimeoutSec", renderTimeoutSec},
                     {"offerTimeoutSec", offerTimeoutSec},
                     {"scheduleEnabled", scheduleEnabled},
                     {"availableFromHour", availableFromHour},
                     {"availableUntilHour", availableUntilHour},
                     {"stopOnActivity", stopOnActivity},
                     {"backgroundRendering", backgroundRendering},
                     {"serverPort", serverPort},
                     {"publicAddress", publicAddress},
                     {"benchmarkPrompted", benchmarkPrompted}});
        data["projectDirectories"] = Json::array();
        for (const auto& path : projectDirectories)
            data["projectDirectories"].push_back(pathUtf8(path));
        data["hiddenProjects"] = Json::array();
        for (const auto& path : hiddenProjects)
            data["hiddenProjects"].push_back(pathUtf8(path));
        // Sérialiser avant de toucher au disque (notamment pour un UTF-8 invalide).
        const auto serialized = data.dump(4) + '\n';
        std::error_code error;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) {
                return SettingsError::WriteFailed;
            }
        }
        static std::atomic<unsigned long long> sequence{0};
        auto temporaryPath = path;
        temporaryPath +=
            "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "." + std::to_string(sequence++) + ".tmp";
        const TemporaryFile temporary{temporaryPath};
        std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return SettingsError::WriteFailed;
        }
#ifndef _WIN32
        // La configuration contient les tokens ; restreindre avant d'écrire (RNF-S2).
        std::filesystem::permissions(temporary.path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            return SettingsError::WriteFailed;
        }
#endif
        output << serialized;
        output.close();
        if (!output) {
            return SettingsError::WriteFailed;
        }
        // Remplacement atomique dans le même dossier ; ne pas tronquer l'ancienne config.
#ifdef _WIN32
        if (!MoveFileExW(temporary.path.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return SettingsError::WriteFailed;
        }
#else
        std::filesystem::rename(temporary.path, path, error);
        if (error) {
            return SettingsError::WriteFailed;
        }
#endif
        return SettingsError::None;
    } catch (const Json::exception&) {
        return SettingsError::InvalidValue;
    } catch (const std::filesystem::filesystem_error&) {
        return SettingsError::WriteFailed;
    }
}

SettingsError AppSettings::update(const std::filesystem::path& path,
                                  const std::function<SettingsError(AppSettings&)>& modify) {
    const std::lock_guard lock(settingsFileMutex);
    AppSettings settings;
    const auto loaded = load(path, settings);
    if (loaded != SettingsError::None && loaded != SettingsError::NotFound)
        return loaded;
    const auto result = modify(settings);
    return result == SettingsError::None ? settings.save(path) : result;
}

} // namespace blendnet
