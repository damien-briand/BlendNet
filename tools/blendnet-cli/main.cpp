#include "../common/BlenderCli.h"
#include "../common/Cli.h"
#include "../common/TransferCli.h"
#include "SourceConnection.h"
#include "Version.h"
#include "config/AppSettings.h"
#include "config/Utf8Path.h"
#include "jobs/JobExecutor.h"
#include "jobs/SimulatedWorker.h"
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
    bool simulateJobs = false, declineJobs = false;
    bool executeJobs = false, keepJobs = false, exitAfterChunk = false;
    unsigned int frameDelay = 30;
    std::string machineName = "simulated-client";
    blendnet::cli::TransferArguments transferArguments;
    blendnet::cli::BlenderArguments blenderArguments;
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
            std::cout << blendnet::cli::transferHelp;
            std::cout << blendnet::cli::blenderHelp;
            std::cout
                << "  --simulate-jobs [--machine-name NAME] [--frame-delay-ms N] [--decline-jobs]\n"
                   "    P5 mock renderer: create 1x1 PNG result archives without Blender.\n";
            std::cout
                << "  --execute-jobs [--use-blender VERSION] [--keep-jobs] [--exit-after-chunk]\n"
                   "    Accept source jobs and render with verified Blender (initial probe: "
                   "4.2.1).\n"
                   "    --keep-jobs preserves workspaces; otherwise remove results after final "
                   "ACK.\n";
            return 0;
        }
        if (argument == "--version") {
            std::cout << "BlendNet " << blendnet::version << '\n';
            return 0;
        }
        if (argument == "--exit-after-transfer") {
            transferArguments.exitAfter = true;
            continue;
        }
        if (argument == "--simulate-jobs") {
            simulateJobs = true;
            continue;
        }
        if (argument == "--execute-jobs") {
            executeJobs = true;
            continue;
        }
        if (argument == "--keep-jobs") {
            keepJobs = true;
            continue;
        }
        if (argument == "--exit-after-chunk") {
            exitAfterChunk = true;
            continue;
        }
        if (argument == "--decline-jobs") {
            declineJobs = true;
            continue;
        }
        if (argument == "--machine-name" && index + 1 < argc) {
            machineName = argv[++index];
            continue;
        }
        if (argument == "--frame-delay-ms" && index + 1 < argc) {
            const auto delay = blendnet::cli::number(argv[++index], 60000);
            if (!delay)
                return 2;
            frameDelay = *delay;
            continue;
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
        } else if (index + 1 < argc && blenderArguments.consume(argument, argv[index + 1])) {
            ++index;
        } else if (index + 1 < argc && transferArguments.consume(argument, argv[index + 1])) {
            ++index;
        } else {
            std::cerr << "Invalid argument. Use --help.\n";
            return 2;
        }
    }

    if (!transferArguments.valid() || (!sourceIndex && !transferArguments.file.path.empty()) ||
        (executeJobs && (!sourceIndex || simulateJobs || declineJobs ||
                         transferArguments.exitAfter || !transferArguments.file.path.empty())) ||
        (exitAfterChunk && !executeJobs))
        return 2;
    if ((!blenderArguments.version.empty() &&
         blenderArguments.mode == blendnet::cli::BlenderArguments::Mode::None && !sourceIndex) ||
        (blenderArguments.mode != blendnet::cli::BlenderArguments::Mode::None && sourceIndex))
        return 2;
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
    if (blenderArguments.mode != blendnet::cli::BlenderArguments::Mode::None)
        return blendnet::cli::runBlender(blenderArguments, settings, configPath);
    if (executeJobs && blenderArguments.version.empty())
        blenderArguments.version = "4.2.1";
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
    options.machineName = machineName;
    options.settingsPath = configPath;
    options.transfers = transferArguments.storage;
    if (options.transfers.directory.empty())
        options.transfers.directory = settings.workDirectory.is_absolute()
                                          ? settings.workDirectory
                                          : configPath.parent_path() / settings.workDirectory;
    const auto workRoot = options.transfers.directory;
    if (executeJobs) {
        options.transfers.directory = workRoot / "transfers";
        options.transfers.quotaDirectory = workRoot;
    }
    options.gpu.backend = settings.computeBackend;
    options.gpu.benchmarkScore = settings.benchmarkScore;
    if (blenderArguments.backend)
        options.gpu.backend = *blenderArguments.backend;
    if (!blenderArguments.version.empty()) {
        blendnet::cli::handleSignals();
        blendnet::cli::BlenderCancellation control;
        blendnet::blender::BlenderInstaller installer(
            blenderArguments.cachePath(settings, configPath), {},
            settings.blenderExecutable.empty() || settings.blenderExecutable.is_absolute()
                ? settings.blenderExecutable : configPath.parent_path() / settings.blenderExecutable);
        const auto binary = installer.ensureVersion(blenderArguments.version, &control.cancelled);
        if (!binary) {
            std::cerr << blendnet::blender::toString(binary.error) << ": " << binary.diagnostic << '\n';
            return 1;
        }
        const auto hardware = blendnet::blender::GpuInfoService::probe(
            *binary.value, options.gpu.backend, &control.cancelled);
        if (!hardware) {
            std::cerr << blendnet::blender::toString(hardware.error) << ": " << hardware.diagnostic << '\n';
            return 1;
        }
        options.gpu = hardware.value->gpu;
        if (settings.computeBackend == options.gpu.backend)
            options.gpu.benchmarkScore = settings.benchmarkScore;
    }
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
    if (!transferArguments.file.path.empty())
        connection.sendFile(transferArguments.file);
    std::unique_ptr<blendnet::jobs::SimulatedWorker> simulation;
    std::unique_ptr<blendnet::jobs::JobExecutor> executor;
    if (executeJobs) {
        blendnet::jobs::ExecutorOptions execution;
        execution.directory = workRoot / "executor";
        execution.quotaDirectory = workRoot;
        execution.quotaBytes = options.transfers.quotaBytes;
        execution.blenderCache = blenderArguments.cachePath(settings, configPath);
        if (!settings.blenderExecutable.empty())
            execution.blenderExecutable = settings.blenderExecutable.is_absolute()
                ? settings.blenderExecutable : configPath.parent_path() / settings.blenderExecutable;
        execution.backend = options.gpu.backend;
        execution.threads = blenderArguments.render.threads;
        execution.renderTimeout = blenderArguments.render.timeout;
        execution.keepCompletedJobs = keepJobs || settings.keepCompletedJobs;
        executor = std::make_unique<blendnet::jobs::JobExecutor>(
            execution,
            blendnet::jobs::JobExecutor::Callbacks{
                [&connection](const blendnet::protocol::Message& message) {
                    connection.sendJobMessage(message);
                },
                [&connection](const blendnet::transfer::FileRequest& file) {
                    connection.sendFile(file);
                },
                [&connection](const std::string& fileId) { connection.cancelFile(fileId); }});
    }
    if (simulateJobs)
        simulation = std::make_unique<blendnet::jobs::SimulatedWorker>(
            configPath.parent_path() / "simulation", std::chrono::milliseconds(frameDelay),
            [&connection](const blendnet::protocol::Message& message) {
                connection.sendJobMessage(message);
            },
            [&connection](const blendnet::transfer::FileRequest& file) {
                connection.sendFile(file);
            },
            declineJobs);
    blendnet::cli::handleSignals();
    bool ready = false;
    bool failed = false;
    bool transferDone = false;
    bool chunkDone = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!blendnet::cli::interrupted &&
           (seconds == 0 || std::chrono::steady_clock::now() < deadline)) {
        for (const auto& event : connection.drainEvents()) {
            if (simulation)
                simulation->handle(event);
            if (executor)
                executor->handle(event);
            if (simulation && event.kind == blendnet::network::EventKind::JobMessage &&
                event.message &&
                std::holds_alternative<blendnet::protocol::TaskAssign>(*event.message))
                std::cout << "SIM_TASK_STARTED\n" << std::flush;
            if (event.kind == blendnet::network::EventKind::Ready) {
                ready = true;
                std::cout << "READY " << connection.snapshot().tlsVersion << '\n' << std::flush;
            } else if (event.kind == blendnet::network::EventKind::TransferProgress &&
                       event.transfer) {
                if (!blendnet::cli::printTransfer(*event.transfer) && !simulateJobs && !executeJobs)
                    failed = true;
                if (event.transfer->state == blendnet::transfer::TransferState::Completed)
                    transferDone = true;
            } else if (event.kind == blendnet::network::EventKind::Error) {
                std::cerr << blendnet::network::toString(event.error) << '\n';
                failed = true;
            } else if (event.kind == blendnet::network::EventKind::Reconnecting) {
                std::cout << "RECONNECTING " << event.retryDelay.count() << " ms\n" << std::flush;
            }
        }
        if (executor)
            for (const auto& event : executor->drainEvents()) {
                std::cout << "EXECUTOR " << blendnet::jobs::toString(event.state) << ' '
                          << event.jobId << ' ' << event.chunkId << '\n'
                          << std::flush;
                if (event.state == blendnet::jobs::ExecutorState::Failed)
                    std::cerr << "Executor: " << event.detail << '\n';
                if (event.state == blendnet::jobs::ExecutorState::Completed)
                    chunkDone = true;
            }
        if (failed || (transferArguments.exitAfter && transferDone) ||
            (exitAfterChunk && chunkDone))
            break;
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
    simulation.reset();
    connection.stop();
    executor.reset();
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
