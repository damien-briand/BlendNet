#include "App.h"
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
    bool smokeTest = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "Usage: blendnet [--config PATH] [--smoke-test] [--version]\n"
                         "  --smoke-test  Render three frames, then exit.\n";
            return 0;
        }
        if (argument == "--version") {
            std::cout << "BlendNet " << blendnet::version << '\n';
            return 0;
        }
        if (argument == "--config" && index + 1 < argc) {
            configPath = blendnet::pathFromUtf8(argv[++index]);
        } else if (argument == "--smoke-test") {
            smokeTest = true;
        } else {
            std::cerr << "Invalid argument. Use --help.\n";
            return 2;
        }
    }

    // RF-10 / RNF-07 : les E/S du socle précèdent la boucle UI (RNF-05).
    blendnet::AppSettings settings;
    const auto loaded = blendnet::AppSettings::load(configPath, settings);
    if (loaded != blendnet::SettingsError::None && loaded != blendnet::SettingsError::NotFound) {
        std::cerr << blendnet::toString(loaded) << '\n';
        return 1;
    }
    if (loaded == blendnet::SettingsError::NotFound) {
        const auto saved = settings.save(configPath);
        if (saved != blendnet::SettingsError::None) {
            std::cerr << blendnet::toString(saved) << '\n';
            return 1;
        }
    }
    blendnet::Logger logger(std::clog, settings.logLevel);
    if (logger.openFile(configPath.parent_path() / "logs" / "blendnet.log") !=
        blendnet::LogError::None) {
        std::cerr << "Cannot open application log.\n";
        return 1;
    }
    if (logger.log(blendnet::LogLevel::Info, "BlendNet starting (P1)") !=
        blendnet::LogError::None) {
        std::cerr << "Cannot write application log.\n";
        return 1;
    }

    blendnet::App app;
    const auto result = app.run(smokeTest ? 3 : 0);
    if (result != blendnet::AppError::None) {
        const auto logged = logger.log(blendnet::LogLevel::Error, "GUI initialization failed");
        if (logged != blendnet::LogError::None) {
            std::cerr << "Cannot write application log.\n";
        }
        std::cerr << "GUI initialization failed (code " << static_cast<int>(result) << ").\n";
        return 1;
    }
    if (logger.log(blendnet::LogLevel::Info, "BlendNet stopped") != blendnet::LogError::None) {
        std::cerr << "Cannot write application log.\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception&) {
        // Ne jamais afficher what() : les exceptions JSON peuvent inclure des secrets.
        std::cerr << "Unexpected application error.\n";
        return 1;
    }
}
