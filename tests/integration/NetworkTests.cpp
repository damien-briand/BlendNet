#include "RenderServer.h"
#include "SourceConnection.h"
#include "TemporaryDirectory.h"
#include "network/Security.h"
#include "protocol/ProtocolCodec.h"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <gtest/gtest.h>
#include <openssl/pem.h>

#include <array>
#include <sstream>
#include <thread>

using namespace blendnet;
using namespace blendnet::network;
using namespace std::chrono_literals;

namespace {
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}
class NetworkIntegration : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    std::ostringstream output;
    Logger logger{output};
    RenderServer server{logger};
    SourceConnection client{logger};
    ServerOptions options;
    ServerIdentity identity;
    ClientOptions clientOptions;

    void SetUp() override {
        ASSERT_EQ(ensureServerIdentity(directory.path() / "identity", identity),
                  NetworkError::None);
        options.port = 0;
        options.token = identity.token;
        options.certificateFile = identity.certificateFile;
        options.privateKeyFile = identity.privateKeyFile;
        options.timings = {40ms, 240ms, 600ms, 20ms, 100ms};
        ASSERT_EQ(server.start(options), NetworkError::None);
        options.port = server.snapshot().port;
        clientOptions.source = {"127.0.0.1", options.port, identity.token, {}};
        clientOptions.settingsPath = directory.path() / "client.json";
        clientOptions.timings = options.timings;
    }
    void TearDown() override {
        client.stop();
        server.stop();
        EXPECT_EQ(output.str().find(identity.token), std::string::npos);
    }
    bool registered() {
        return waitFor([this] {
            return client.snapshot().state == ConnectionState::Ready &&
                   server.snapshot().clients.size() == 1;
        });
    }
    bool serverError(NetworkError error) {
        return waitFor([this, error] {
            for (const auto& event : server.drainEvents())
                if (event.error == error)
                    return true;
            return false;
        });
    }
};

// Pair de test volontairement non authentifié ; le transport reste toujours TLS.
class RawPeer {
  public:
    asio::io_context io;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    asio::ssl::stream<asio::ip::tcp::socket> stream{io, tls};
    explicit RawPeer(std::uint16_t port) {
        stream.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
        stream.handshake(asio::ssl::stream_base::client);
    }
    void send(const protocol::Message& message) {
        const auto bytes = protocol::ProtocolCodec::encode(message);
        if (!bytes)
            throw std::runtime_error("Invalid raw peer fixture");
        asio::write(stream, asio::buffer(*bytes.value));
    }
    protocol::Message receive() {
        std::array<std::uint8_t, protocol::headerBytes> bytes{};
        asio::read(stream, asio::buffer(bytes));
        const auto header = protocol::ProtocolCodec::decodeHeader(bytes);
        if (!header)
            throw std::runtime_error("Invalid server header");
        protocol::Bytes body(header.value->payloadBytes);
        asio::read(stream, asio::buffer(body));
        const auto message = protocol::ProtocolCodec::decodePayload(*header.value, body);
        if (!message)
            throw std::runtime_error("Invalid server message");
        return *message.value;
    }
};

protocol::Message readMessage(asio::ssl::stream<asio::ip::tcp::socket>& stream) {
    std::array<std::uint8_t, protocol::headerBytes> bytes{};
    asio::read(stream, asio::buffer(bytes));
    const auto header = protocol::ProtocolCodec::decodeHeader(bytes);
    if (!header)
        throw std::runtime_error("Invalid fixture header");
    protocol::Bytes body(header.value->payloadBytes);
    asio::read(stream, asio::buffer(body));
    const auto message = protocol::ProtocolCodec::decodePayload(*header.value, body);
    if (!message)
        throw std::runtime_error("Invalid fixture message");
    return *message.value;
}
} // namespace

TEST_F(NetworkIntegration, TlsRegistrationPersistsPinExchangesHeartbeatsAndPreferences) {
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(registered());
    const auto snapshot = server.snapshot();
    ASSERT_EQ(snapshot.clients.size(), 1);
    EXPECT_EQ(snapshot.clients[0].gpu.backend, "CPU");
    EXPECT_EQ(snapshot.clients[0].userName, "cli-user");
    EXPECT_EQ(snapshot.clients[0].id.size(), 36);
    EXPECT_TRUE(client.snapshot().tlsVersion == "TLSv1.3" ||
                client.snapshot().tlsVersion == "TLSv1.2");
    AppSettings persisted;
    ASSERT_EQ(AppSettings::load(clientOptions.settingsPath, persisted), SettingsError::None);
    ASSERT_EQ(persisted.knownSources.size(), 1);
    EXPECT_EQ(persisted.knownSources[0].certificateSha256, client.snapshot().certificateSha256);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().pongsReceived >= 2; }));
    clientOptions.preferences.availability = "DND";
    client.updatePreferences(clientOptions.preferences);
    ASSERT_TRUE(waitFor([this] {
        const auto state = server.snapshot();
        return !state.clients.empty() && state.clients[0].preferences.availability == "DND";
    }));
}
TEST_F(NetworkIntegration, WrongTokenIsTerminalAndDoesNotPersistAnUnpairedCertificate) {
    clientOptions.source.token = std::string(64, 'x');
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::AuthenticationFailed);
    EXPECT_EQ(client.snapshot().retryCount, 0);
    EXPECT_FALSE(std::filesystem::exists(clientOptions.settingsPath));
    EXPECT_TRUE(server.snapshot().clients.empty());
}
TEST_F(NetworkIntegration, WrongProtocolVersionIsExplicitlyRejected) {
    clientOptions.protocolVersion = 42;
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::VersionMismatch);
    EXPECT_FALSE(std::filesystem::exists(clientOptions.settingsPath));
}
TEST_F(NetworkIntegration, CertificateReplacementIsRejectedWithoutOverwritingThePin) {
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(registered());
    const auto previous = client.snapshot().certificateSha256;
    client.stop();
    server.stop();
    ServerIdentity other;
    ASSERT_EQ(ensureServerIdentity(directory.path() / "other-identity", other), NetworkError::None);
    options.certificateFile = other.certificateFile;
    options.privateKeyFile = other.privateKeyFile;
    ASSERT_EQ(server.start(options), NetworkError::None);
    clientOptions.source.certificateSha256 = previous;
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::CertificateMismatch);
    EXPECT_EQ(client.snapshot().retryCount, 0);
    EXPECT_TRUE(server.snapshot().clients.empty());
    AppSettings persisted;
    ASSERT_EQ(AppSettings::load(clientOptions.settingsPath, persisted), SettingsError::None);
    EXPECT_EQ(persisted.knownSources.at(0).certificateSha256, previous);
}
TEST_F(NetworkIntegration, ReconnectsAfterServerRestartWithTheSamePinnedCertificate) {
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(registered());
    const auto pin = client.snapshot().certificateSha256;
    server.stop();
    ASSERT_TRUE(waitFor([this] { return client.snapshot().retryCount >= 2; }));
    auto events = client.drainEvents();
    std::vector<std::chrono::milliseconds> delays;
    for (const auto& event : events)
        if (event.kind == EventKind::Reconnecting)
            delays.push_back(event.retryDelay);
    ASSERT_GE(delays.size(), 2);
    EXPECT_EQ(delays[0], 20ms);
    EXPECT_EQ(delays[1], 40ms);
    ASSERT_EQ(server.start(options), NetworkError::None);
    ASSERT_TRUE(waitFor([this] {
        return client.snapshot().readyCount >= 2 && server.snapshot().clients.size() == 1;
    }));
    EXPECT_EQ(client.snapshot().certificateSha256, pin);
}
TEST_F(NetworkIntegration, KickStopsTheClientWithoutAutomaticReconnect) {
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(registered());
    server.disconnect(server.snapshot().clients.at(0).id);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::Kicked);
    EXPECT_EQ(client.snapshot().retryCount, 0);
}
TEST_F(NetworkIntegration, PinWriteFailureFailsClosedBeforeGpuRegistration) {
    const auto obstruction = directory.path() / "file";
    test::writeFile(obstruction, "not a directory");
    clientOptions.settingsPath = obstruction / "settings.json";
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::PinPersistenceFailed);
    EXPECT_TRUE(server.snapshot().clients.empty());
}
TEST_F(NetworkIntegration, MessageBeforeHelloAndMalformedHeaderAreRejected) {
    {
        RawPeer peer(options.port);
        peer.send(protocol::Ping{});
        ASSERT_TRUE(serverError(NetworkError::ProtocolViolation));
    }
    {
        RawPeer peer(options.port);
        std::array<std::uint8_t, 12> garbage{};
        asio::write(peer.stream, asio::buffer(garbage));
        ASSERT_TRUE(serverError(NetworkError::ProtocolViolation));
    }
    EXPECT_TRUE(server.snapshot().clients.empty());
}
TEST_F(NetworkIntegration, MissingPongDisconnectsAnAuthenticatedPeer) {
    RawPeer peer(options.port);
    peer.send(protocol::Hello{1, "silent", "test", identity.token});
    EXPECT_TRUE(std::holds_alternative<protocol::HelloOk>(peer.receive()));
    peer.send(clientOptions.gpu);
    peer.send(clientOptions.preferences);
    ASSERT_TRUE(waitFor([this] { return server.snapshot().clients.size() == 1; }));
    ASSERT_TRUE(serverError(NetworkError::Timeout));
    EXPECT_TRUE(server.snapshot().clients.empty());
}
TEST_F(NetworkIntegration, SilentTlsHandshakeHasADeadlineAndStopCancelsPendingOperations) {
    asio::io_context io;
    asio::ip::tcp::socket peer(io);
    peer.connect({asio::ip::make_address("127.0.0.1"), options.port});
    ASSERT_TRUE(serverError(NetworkError::Timeout));
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    const auto started = std::chrono::steady_clock::now();
    client.stop();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

// P9 / RNF-S1: malformed input must close only the offending TLS connection.
TEST_F(NetworkIntegration, MalformedPayloadsOfEveryMessageTypeCloseCleanly) {
    for (const auto type : {0, 1, 2, 3, 4, 5, 6, 10, 11, 12, 20, 21, 22, 23, 24, 25, 26,
                            30, 31, 32, 33, 34}) {
        SCOPED_TRACE(type);
        RawPeer peer(options.port);
        peer.send(protocol::Hello{1, "fuzz", "test", identity.token});
        ASSERT_TRUE(std::holds_alternative<protocol::HelloOk>(peer.receive()));
        peer.send(clientOptions.gpu);
        peer.send(clientOptions.preferences);
        ASSERT_TRUE(waitFor([this] { return server.snapshot().clients.size() == 1; }));
        protocol::Bytes bytes{std::uint8_t(protocol::protocolMagic >> 8),
                              std::uint8_t(protocol::protocolMagic), 0, std::uint8_t(type),
                              0, 0, 0, 0, 0, 0, 0, 1, '{'};
        asio::write(peer.stream, asio::buffer(bytes));
        ASSERT_TRUE(serverError(NetworkError::ProtocolViolation));
        EXPECT_TRUE(server.snapshot().clients.empty());
    }
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    EXPECT_TRUE(registered());
}

TEST_F(NetworkIntegration, AuthenticatedButIncompleteRegistrationStillTimesOut) {
    RawPeer peer(options.port);
    peer.send(protocol::Hello{1, "slow", "test", identity.token});
    ASSERT_TRUE(std::holds_alternative<protocol::HelloOk>(peer.receive()));
    ASSERT_TRUE(serverError(NetworkError::Timeout));
    EXPECT_TRUE(server.snapshot().clients.empty());
}

TEST_F(NetworkIntegration, IncompleteFrameDoesNotKeepTheConnectionAlive) {
    RawPeer peer(options.port);
    peer.send(protocol::Hello{1, "slow", "test", identity.token});
    ASSERT_TRUE(std::holds_alternative<protocol::HelloOk>(peer.receive()));
    peer.send(clientOptions.gpu);
    peer.send(clientOptions.preferences);
    ASSERT_TRUE(waitFor([this] { return server.snapshot().clients.size() == 1; }));
    auto bytes = protocol::ProtocolCodec::encode(protocol::Ping{});
    ASSERT_TRUE(bytes);
    // A valid header whose announced payload is never completed.
    bytes.value->resize(protocol::headerBytes);
    asio::write(peer.stream, asio::buffer(*bytes.value));
    ASSERT_TRUE(serverError(NetworkError::Timeout));
    EXPECT_TRUE(server.snapshot().clients.empty());
}

TEST_F(NetworkIntegration, ClientTimesOutWhenTheSourceStopsAnsweringHeartbeats) {
    server.stop();
    asio::io_context io;
    asio::ssl::context tls(asio::ssl::context::tls_server);
    tls.use_certificate_chain_file(identity.certificateFile.string());
    tls.use_private_key_file(identity.privateKeyFile.string(), asio::ssl::context::pem);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
    clientOptions.source.port = acceptor.local_endpoint().port();
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    acceptor.accept(stream.next_layer());
    stream.handshake(asio::ssl::stream_base::server);
    ASSERT_TRUE(std::holds_alternative<protocol::Hello>(readMessage(stream)));
    const auto hello = protocol::ProtocolCodec::encode(protocol::HelloOk{1, "silent-source"});
    ASSERT_TRUE(hello);
    asio::write(stream, asio::buffer(*hello.value));
    EXPECT_TRUE(std::holds_alternative<protocol::GpuInfo>(readMessage(stream)));
    EXPECT_TRUE(std::holds_alternative<protocol::ClientPrefs>(readMessage(stream)));
    ASSERT_TRUE(waitFor([this] { return client.snapshot().retryCount > 0; }));
    bool timeout = false;
    for (const auto& event : client.drainEvents())
        if (event.kind == EventKind::Disconnected && event.error == NetworkError::Timeout)
            timeout = true;
    EXPECT_TRUE(timeout);
}

TEST_F(NetworkIntegration, ExpiredCertificateIsRejectedBeforeAuthentication) {
    server.stop();
    const std::unique_ptr<BIO, decltype(&BIO_free)> certificateFile(
        BIO_new_file(identity.certificateFile.string().c_str(), "rb"), BIO_free);
    const std::unique_ptr<BIO, decltype(&BIO_free)> keyFile(
        BIO_new_file(identity.privateKeyFile.string().c_str(), "rb"), BIO_free);
    ASSERT_TRUE(certificateFile);
    ASSERT_TRUE(keyFile);
    const std::unique_ptr<X509, decltype(&X509_free)> certificate(
        PEM_read_bio_X509(certificateFile.get(), nullptr, nullptr, nullptr), X509_free);
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(keyFile.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    ASSERT_TRUE(certificate);
    ASSERT_TRUE(key);
    ASSERT_NE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), -60), nullptr);
    ASSERT_GT(X509_sign(certificate.get(), key.get(), EVP_sha256()), 0);
    const std::unique_ptr<BIO, decltype(&BIO_free)> encoded(BIO_new(BIO_s_mem()), BIO_free);
    ASSERT_EQ(PEM_write_bio_X509(encoded.get(), certificate.get()), 1);
    char* contents = nullptr;
    const auto size = BIO_get_mem_data(encoded.get(), &contents);
    test::writeFile(identity.certificateFile,
                    std::string_view(contents, static_cast<std::size_t>(size)));
    ASSERT_EQ(server.start(options), NetworkError::None);
    ASSERT_EQ(client.start(clientOptions), NetworkError::None);
    ASSERT_TRUE(waitFor([this] { return client.snapshot().state == ConnectionState::Stopped; }));
    EXPECT_EQ(client.snapshot().error, NetworkError::CertificateRejected);
    EXPECT_TRUE(server.snapshot().clients.empty());
    EXPECT_FALSE(std::filesystem::exists(clientOptions.settingsPath));
}
