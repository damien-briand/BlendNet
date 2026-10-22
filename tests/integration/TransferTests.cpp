#include "RenderServer.h"
#include "SourceConnection.h"
#include "TemporaryDirectory.h"
#include "network/Security.h"
#include "protocol/ProtocolCodec.h"
#include "transfer/TransferEngine.h"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <sstream>
#include <thread>

using namespace blendnet;
using namespace blendnet::network;
using namespace blendnet::transfer;
using namespace std::chrono_literals;
namespace {
template <typename Predicate>
bool await(Predicate predicate, std::chrono::milliseconds duration = 15000ms) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
std::optional<TransferProgress> find(const std::vector<TransferProgress>& values,
                                     const std::string& id, Direction direction,
                                     TransferState state) {
    for (const auto& value : values)
        if (value.fileId == id && value.direction == direction && value.state == state)
            return value;
    return {};
}
class TransferIntegration : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    std::ostringstream output;
    Logger logger{output};
    RenderServer server{logger};
    SourceConnection client{logger};
    ServerOptions serverOptions;
    ClientOptions clientOptions;
    ServerIdentity identity;
    void SetUp() override {
        ASSERT_EQ(ensureServerIdentity(directory.path() / "identity", identity),
                  NetworkError::None);
        serverOptions.port = 0;
        serverOptions.certificateFile = identity.certificateFile;
        serverOptions.privateKeyFile = identity.privateKeyFile;
        serverOptions.token = identity.token;
        serverOptions.timings = {50ms, 2000ms, 2000ms, 100ms, 500ms};
        serverOptions.transfers = {directory.path() / "source-received", 1024ULL * 1024 * 1024};
        ASSERT_EQ(server.start(serverOptions), NetworkError::None);
        serverOptions.port = server.snapshot().port;
        clientOptions.source = {"127.0.0.1", serverOptions.port, identity.token, {}};
        clientOptions.settingsPath = directory.path() / "settings.json";
        clientOptions.timings = serverOptions.timings;
        clientOptions.transfers = {directory.path() / "client-received", 1024ULL * 1024 * 1024};
    }
    void TearDown() override {
        client.stop();
        server.stop();
        EXPECT_EQ(output.str().find(identity.token), std::string::npos);
    }
    void connect() {
        ASSERT_EQ(client.start(clientOptions), NetworkError::None);
        ASSERT_TRUE(await([this] {
            return client.snapshot().state == ConnectionState::Ready &&
                   server.snapshot().clients.size() == 1;
        }));
    }
    std::filesystem::path fixture(std::size_t megabytes) {
        const auto path = directory.path() / ("payload-" + std::to_string(megabytes) + ".zip");
        std::ofstream outputFile(path, std::ios::binary);
        protocol::Bytes buffer(1024 * 1024);
        for (std::size_t i = 0; i < buffer.size(); ++i)
            buffer[i] = static_cast<std::uint8_t>(i * 17);
        for (std::size_t count = 0; count < megabytes; ++count)
            outputFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        if (!outputFile)
            throw std::runtime_error("Fixture failed");
        return path;
    }
};
protocol::Message read(asio::ssl::stream<asio::ip::tcp::socket>& stream) {
    std::array<std::uint8_t, protocol::headerBytes> header{};
    asio::read(stream, asio::buffer(header));
    const auto parsed = protocol::ProtocolCodec::decodeHeader(header);
    if (!parsed)
        throw std::runtime_error("Invalid test header");
    protocol::Bytes body(parsed.value->payloadBytes);
    asio::read(stream, asio::buffer(body));
    const auto message = protocol::ProtocolCodec::decodePayload(*parsed.value, body);
    if (!message)
        throw std::runtime_error("Invalid test message");
    return *message.value;
}
void write(asio::ssl::stream<asio::ip::tcp::socket>& stream, const protocol::Message& message) {
    const auto wire = protocol::ProtocolCodec::encode(message);
    if (!wire)
        throw std::runtime_error("Invalid test message");
    asio::write(stream, asio::buffer(*wire.value));
}
} // namespace
TEST_F(TransferIntegration, SimultaneousBidirectionalStreamingAndFinalAcks) {
    const auto file = fixture(16);
    connect();
    client.sendFile({file, "upload_RESULT_ZIP", "RESULT_ZIP"});
    server.sendFile(server.snapshot().clients.at(0).id, {file, "download_PROJECT_ZIP"});
    ASSERT_TRUE(await([this] {
        return find(client.snapshot().transfers, "upload_RESULT_ZIP", Direction::Sending,
                    TransferState::Completed) &&
               find(client.snapshot().transfers, "download_PROJECT_ZIP", Direction::Receiving,
                    TransferState::Completed) &&
               find(server.snapshot().transfers, "download_PROJECT_ZIP", Direction::Sending,
                    TransferState::Completed);
    }));
    const auto upload = find(server.snapshot().transfers, "upload_RESULT_ZIP", Direction::Receiving,
                             TransferState::Completed);
    ASSERT_TRUE(upload);
    const auto download = find(client.snapshot().transfers, "download_PROJECT_ZIP",
                               Direction::Receiving, TransferState::Completed);
    ASSERT_TRUE(download);
    const auto hash = sha256File(file);
    ASSERT_TRUE(hash);
    EXPECT_EQ(sha256File(upload->path).value, hash.value);
    EXPECT_EQ(sha256File(download->path).value, hash.value);
    EXPECT_EQ(client.snapshot().state, ConnectionState::Ready);
}
TEST_F(TransferIntegration, UploadResumesAfterReceiverServerIsDestroyedAndRecreated) {
    const auto file = fixture(128);
    connect();
    client.sendFile({file, "resume_RESULT_ZIP", "RESULT_ZIP"});
    ASSERT_TRUE(await([this] {
        const auto progress = find(server.snapshot().transfers, "resume_RESULT_ZIP",
                                   Direction::Receiving, TransferState::Transferring);
        return progress && progress->completedBytes >= 32 * 1024 * 1024;
    }));
    server.stop();
    ASSERT_TRUE(await([this] { return client.snapshot().retryCount > 0; }));
    ASSERT_EQ(server.start(serverOptions), NetworkError::None);
    ASSERT_TRUE(await([this] {
        return find(client.snapshot().transfers, "resume_RESULT_ZIP", Direction::Sending,
                    TransferState::Completed)
            .has_value();
    }));
    const auto sender = find(client.snapshot().transfers, "resume_RESULT_ZIP", Direction::Sending,
                             TransferState::Completed);
    ASSERT_TRUE(sender);
    EXPECT_GT(sender->resumedBytes, 0);
    EXPECT_LT(sender->resumedBytes, sender->totalBytes);
    const auto received = find(server.snapshot().transfers, "resume_RESULT_ZIP",
                               Direction::Receiving, TransferState::Completed);
    ASSERT_TRUE(received);
    EXPECT_EQ(sha256File(received->path).value, sha256File(file).value);
}
TEST_F(TransferIntegration, DownloadResumesAutomaticallyAfterTransportCut) {
    const auto file = fixture(128);
    connect();
    const auto firstSession = server.snapshot().clients.at(0).id;
    server.sendFile(firstSession, {file, "resume_PROJECT_ZIP"});
    ASSERT_TRUE(await([this] {
        const auto progress = find(client.snapshot().transfers, "resume_PROJECT_ZIP",
                                   Direction::Receiving, TransferState::Transferring);
        return progress && progress->completedBytes >= 32 * 1024 * 1024;
    }));
    server.interrupt(firstSession);
    ASSERT_TRUE(await([this] {
        return client.snapshot().readyCount >= 2 &&
               find(client.snapshot().transfers, "resume_PROJECT_ZIP", Direction::Receiving,
                    TransferState::Completed)
                   .has_value() &&
               find(server.snapshot().transfers, "resume_PROJECT_ZIP", Direction::Sending,
                    TransferState::Completed)
                   .has_value();
    }));
    const auto received = find(client.snapshot().transfers, "resume_PROJECT_ZIP",
                               Direction::Receiving, TransferState::Completed);
    ASSERT_TRUE(received);
    EXPECT_GT(received->resumedBytes, 0);
    EXPECT_LT(received->resumedBytes, received->totalBytes);
    EXPECT_EQ(sha256File(received->path).value, sha256File(file).value);
}
TEST_F(TransferIntegration, QuotaFailureIsReportedWithoutClosingTheTlsSession) {
    server.stop();
    serverOptions.transfers.quotaBytes = 1024;
    ASSERT_EQ(server.start(serverOptions), NetworkError::None);
    const auto file = fixture(2);
    connect();
    client.sendFile({file, "quota_RESULT_ZIP", "RESULT_ZIP"});
    ASSERT_TRUE(await([this] {
        return find(client.snapshot().transfers, "quota_RESULT_ZIP", Direction::Sending,
                    TransferState::Failed)
            .has_value();
    }));
    const auto progress = find(client.snapshot().transfers, "quota_RESULT_ZIP", Direction::Sending,
                               TransferState::Failed);
    ASSERT_TRUE(progress);
    EXPECT_EQ(progress->error, TransferError::DiskFull);
    EXPECT_EQ(client.snapshot().state, ConnectionState::Ready);
}
TEST_F(TransferIntegration, ForgedChecksumProducesFileErrorAndNoValidatedArchive) {
    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    asio::ssl::stream<asio::ip::tcp::socket> raw(io, context);
    raw.next_layer().connect({asio::ip::make_address("127.0.0.1"), serverOptions.port});
    raw.handshake(asio::ssl::stream_base::client);
    write(raw, protocol::Hello{1, "raw", "raw", identity.token});
    ASSERT_TRUE(std::holds_alternative<protocol::HelloOk>(read(raw)));
    write(raw, clientOptions.gpu);
    write(raw, clientOptions.preferences);
    write(raw, protocol::FileBegin{"bad_PROJECT_ZIP", "PROJECT_ZIP", 3, std::string(64, '0'), 3});
    ASSERT_TRUE(std::holds_alternative<protocol::FileAck>(read(raw)));
    write(raw, protocol::FileData{"bad_PROJECT_ZIP", 0, {'a', 'b', 'c'}});
    ASSERT_TRUE(std::holds_alternative<protocol::FileAck>(read(raw)));
    write(raw, protocol::FileEnd{"bad_PROJECT_ZIP", std::string(64, '0')});
    const auto result = read(raw);
    ASSERT_TRUE(std::holds_alternative<protocol::FileErr>(result));
    EXPECT_EQ(std::get<protocol::FileErr>(result).reason, "sha_mismatch");
    for (const auto& entry : std::filesystem::directory_iterator(serverOptions.transfers.directory))
        EXPECT_NE(entry.path().extension(), ".zip");
}
