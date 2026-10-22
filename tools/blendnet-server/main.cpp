#include "../common/Cli.h"
#include "../common/TransferCli.h"
#include "RenderServer.h"
#include "Version.h"
#include "config/Utf8Path.h"
#include "network/Security.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

namespace {
int run(int argc, char** argv) {
    using namespace blendnet;
    using namespace std::chrono;
    auto directory = AppSettings::defaultPath().parent_path() / "server";
    network::ServerOptions options;
    unsigned int seconds = 0;
    cli::TransferArguments transferArguments;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "Usage: blendnet-server-cli [--identity-dir PATH] [--bind IP] [--port N] "
                         "[--seconds N]\n"
                         "  Creates a private server identity and pairing-token.txt if absent.\n"
                         "  Default bind: 127.0.0.1. Use --bind 0.0.0.0 for Internet access.\n";
            std::cout << cli::transferHelp;
            return 0;
        }
        if (argument == "--version") {
            std::cout << "BlendNet " << version << '\n';
            return 0;
        }
        if (argument == "--exit-after-transfer") {
            transferArguments.exitAfter = true;
            continue;
        }
        if (index + 1 >= argc)
            return 2;
        const std::string_view value(argv[++index]);
        if (argument == "--identity-dir")
            directory = pathFromUtf8(value);
        else if (argument == "--bind")
            options.bindAddress = value;
        else if (argument == "--port") {
            const auto port = cli::number(value, 65535);
            if (!port)
                return 2;
            options.port = static_cast<std::uint16_t>(*port);
        } else if (argument == "--seconds") {
            const auto duration = cli::number(value, 86400);
            if (!duration)
                return 2;
            seconds = *duration;
        } else if (!transferArguments.consume(argument, value))
            return 2;
    }
    if (!transferArguments.valid())
        return 2;
    network::ServerIdentity identity;
    const auto prepared = network::ensureServerIdentity(directory, identity);
    if (prepared != network::NetworkError::None) {
        std::cerr << network::toString(prepared) << '\n';
        return 1;
    }
    options.certificateFile = identity.certificateFile;
    options.privateKeyFile = identity.privateKeyFile;
    options.token = identity.token;
    options.transfers = transferArguments.storage;
    if (options.transfers.directory.empty())
        options.transfers.directory = directory / "received";
    Logger logger(std::clog);
    RenderServer server(logger);
    const auto started = server.start(options);
    if (started != network::NetworkError::None) {
        std::cerr << network::toString(started) << '\n';
        return 1;
    }
    cli::handleSignals();
    std::cout << "LISTENING " << server.snapshot().port << '\n' << std::flush;
    std::cout << "Pairing token file: " << identity.tokenFile << '\n' << std::flush;
    bool failed = false;
    bool transferDone = false;
    const auto deadline = steady_clock::now() + std::chrono::seconds(seconds);
    while (!cli::interrupted && (seconds == 0 || steady_clock::now() < deadline)) {
        for (const auto& event : server.drainEvents()) {
            if (event.kind == network::EventKind::Ready) {
                std::cout << "CLIENT_READY\n" << std::flush;
                if (!transferArguments.file.path.empty())
                    server.sendFile(event.sessionId, transferArguments.file);
            } else if (event.kind == network::EventKind::TransferProgress && event.transfer) {
                if (!cli::printTransfer(*event.transfer))
                    failed = true;
                if (event.transfer->state == transfer::TransferState::Completed)
                    transferDone = true;
            }
        }
        if (failed || (transferArguments.exitAfter && transferDone))
            break;
        std::this_thread::sleep_for(milliseconds(20));
    }
    server.stop();
    return failed ? 1 : 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception&) {
        std::cerr << "Unexpected server error.\n";
        return 1;
    }
}
