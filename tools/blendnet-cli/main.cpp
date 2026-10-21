#include "../common/Cli.h"
#include "SourceConnection.h"
#include "Version.h"
#include "config/AppSettings.h"
#include "config/Utf8Path.h"
#include "log/Logger.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

int run(int argc, char** argv) {
    auto configPath = blendnet::AppSettings::defaultPath();
    bool initialize = false;
    std::optional<unsigned int> sourceIndex;
    unsigned int seconds = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout
                << "Usage: blendnet-cli [--config PATH] [--init-config] [--connect INDEX] "
                   "[--seconds N] [--version]\n"
                   "  Validate settings without opening a window. Missing settings use defaults.\n"
                   "  --init-config  Create missing settings; preserve an existing file.\n"
                   "  --connect INDEX  Connect to a known source (zero-based index).\n"
                   "  --seconds N  Exit after N seconds (0: wait for Ctrl+C).\n";
            return 0;
        }
        if (argument == "--version") {
            std::cout << "BlendNet " << blendnet::version << '\n';
            return 0;
        }
        if (argument == "--config" && index + 1 < argc) {
            configPath = blendnet::pathFromUtf8(argv[++index]);
        } else if (argument == "--connect" && index + 1 < argc) {
            sourceIndex = blendnet::cli::number(argv[++index], 65535);
            if (!sourceIndex)
                return 2;
        } else if (argument == "--seconds" && index + 1 < argc) {
            const auto duration = blendnet::cli::number(argv[++index], 86400);
            if (!duration)
                return 2;
            seconds = *duration;
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
    if (!sourceIndex) {
        std::cout << "BlendNet " << blendnet::version << " — headless foundation ready.\n";
        return 0;
    }
    if (*sourceIndex >= settings.knownSources.size()) {
        std::cerr << "Known source index out of range.\n";
        return 2;
    }
    blendnet::network::ClientOptions options;
    options.source = settings.knownSources[*sourceIndex];
    options.settingsPath = configPath;
    options.gpu.backend = settings.computeBackend;
    options.gpu.benchmarkScore = settings.benchmarkScore;
#ifdef _WIN32
    options.gpu.os = "windows";
#elif defined(__APPLE__)
    options.gpu.os = "macos";
#endif
    blendnet::SourceConnection connection(logger);
    const auto started = connection.start(options);
    if (started != blendnet::network::NetworkError::None) {
        std::cerr << blendnet::network::toString(started) << '\n';
        return 1;
    }
    blendnet::cli::handleSignals();
    bool ready = false;
    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!blendnet::cli::interrupted &&
           (seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
        for (const auto& event : connection.drainEvents()) {
            if (event.kind == blendnet::network::EventKind::Ready) {
                ready = true;
                std::cout << "READY " << connection.snapshot().tlsVersion << '\n' << std::flush;
            } else if (event.kind == blendnet::network::EventKind::Error) {
                std::cerr << blendnet::network::toString(event.error) << '\n';
                failed = true;
            } else if (event.kind == blendnet::network::EventKind::Reconnecting) {
                std::cout << "RECONNECTING " << event.retryDelay.count() << " ms\n" << std::flush;
            }
        }
        if (connection.snapshot().state == blendnet::network::ConnectionState::Stopped)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto finalState = connection.snapshot();
    if (finalState.state == blendnet::network::ConnectionState::Stopped &&
        finalState.error != blendnet::network::NetworkError::None &&
        finalState.error != blendnet::network::NetworkError::Stopped) {
        if (!failed)
            std::cerr << blendnet::network::toString(finalState.error) << '\n';
        failed = true;
    }
    connection.stop();
    return ready && !failed ? 0 : 1;
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
