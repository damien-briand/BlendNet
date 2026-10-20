#include "Version.h"
#include "config/AppSettings.h"
#include "config/Utf8Path.h"
#include "log/Logger.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int run(int argc, char** argv) {
    auto configPath = blendnet::AppSettings::defaultPath();
    bool initialize = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout
                << "Usage: blendnet-cli [--config PATH] [--init-config] [--version]\n"
                   "  Validate settings without opening a window. Missing settings use defaults.\n"
                   "  --init-config  Create missing settings; preserve an existing file.\n"
                   "  Network simulation will be available in P3.\n";
            return 0;
        }
        if (argument == "--version") {
            std::cout << "BlendNet " << blendnet::version << '\n';
            return 0;
        }
        if (argument == "--config" && index + 1 < argc) {
            configPath = blendnet::pathFromUtf8(argv[++index]);
        } else if (argument == "--init-config") {
            initialize = true;
        } else {
            std::cerr << "Invalid argument. Use --help.\n";
            return 2;
        }
    }

    blendnet::AppSettings settings;
    const auto loaded = blendnet::AppSettings::load(configPath, settings);
    if (loaded != blendnet::SettingsError::None && loaded != blendnet::SettingsError::NotFound) {
        std::cerr << blendnet::toString(loaded) << '\n';
        return 1;
    }
    if (initialize && loaded == blendnet::SettingsError::NotFound) {
        const auto saved = settings.save(configPath);
        if (saved != blendnet::SettingsError::None) {
            std::cerr << blendnet::toString(saved) << '\n';
            return 1;
        }
    }
    blendnet::Logger logger(std::clog, settings.logLevel);
    if (logger.log(blendnet::LogLevel::Info, "Headless settings check completed (P1)") !=
        blendnet::LogError::None) {
        std::cerr << "Cannot write application log.\n";
        return 1;
    }
    std::cout << "BlendNet " << blendnet::version << " — P1 headless foundation ready.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception&) {
        std::cerr << "Unexpected application error.\n";
        return 1;
    }
}
