#include "network/ClientSocket.h"
#include "network/Security.h"
#include "network/TlsChannel.h"
#include "network/TransferLink.h"

#include <openssl/x509v3.h>

#include <map>
#include <thread>

namespace blendnet::network {
using detail::TlsChannel;
using namespace std::chrono;

struct ClientSocket::Impl {
    Logger& logger;
    ClientOptions options;
    asio::io_context io;
    asio::thread_pool workers{2};
    asio::strand<asio::thread_pool::executor_type> transferWorker{asio::make_strand(workers)};
    asio::ssl::context tls{asio::ssl::context::tls_client};
    asio::ip::tcp::resolver resolver{io};
    asio::steady_timer deadline{io};
    asio::steady_timer heartbeat{io};
    asio::steady_timer retry{io};
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(io)};
    std::jthread thread;
    std::shared_ptr<TlsChannel> channel;
    std::shared_ptr<detail::TransferLink> transfers;
    std::map<std::string, transfer::FileRequest> pendingFiles;
    ReconnectPolicy policy;
    bool stopping = false;
    bool awaitingHello = true;
    bool certificateRejected = false;
    bool certificateMismatch = false;
    std::string fingerprint;
    steady_clock::time_point lastPong;
    mutable std::mutex mutex;
    ClientSnapshot state;
    EventQueue<NetworkEvent> events;

    explicit Impl(Logger& log) : logger(log) {}
    ~Impl() { stop(); }
    void setState(ConnectionState value, NetworkError error = NetworkError::None) {
        const std::lock_guard lock(mutex);
        state.state = value;
        state.error = error;
    }
    NetworkError start(const ClientOptions& settings) {
        options = settings;
        if (!options.timings.valid() || options.source.host.empty() || options.source.port < 1 ||
            options.source.port > 65535 || options.settingsPath.empty())
            return NetworkError::InvalidOptions;
        AppSettings validator;
        validator.knownSources.push_back(options.source);
        if (validator.validate() != SettingsError::None ||
            !protocol::ProtocolCodec::encode(protocol::Hello{options.protocolVersion,
                                                             options.userName, options.machineName,
                                                             options.source.token}) ||
            !protocol::ProtocolCodec::encode(options.gpu) ||
            !protocol::ProtocolCodec::encode(options.preferences))
            return NetworkError::InvalidOptions;
        try {
            if (!detail::configureTls(tls))
                return NetworkError::TlsConfiguration;
        } catch (const std::exception&) {
            return NetworkError::TlsConfiguration;
        }
        policy = ReconnectPolicy(options.timings.retryInitial, options.timings.retryMaximum);
        asio::post(io, [this] { connect(); });
        thread = std::jthread([this] { io.run(); });
        return NetworkError::None;
    }
    void connect() {
        if (stopping)
            return;
        awaitingHello = true;
        certificateMismatch = false;
        certificateRejected = false;
        fingerprint.clear();
        setState(ConnectionState::Connecting);
        channel = std::make_shared<TlsChannel>(io, tls);
        const auto current = channel;
        current->callbacks(
            [this, weak = std::weak_ptr<TlsChannel>(current)](protocol::Message message) {
                if (const auto active = weak.lock(); active && active == channel)
                    receive(std::move(message));
            },
            [this, weak = std::weak_ptr<TlsChannel>(current)](NetworkError error) {
                if (const auto active = weak.lock(); active && active == channel)
                    failed(error);
            });
        current->stream().set_verify_mode(asio::ssl::verify_peer);
        current->stream().set_verify_callback([this](bool, asio::ssl::verify_context& context) {
            auto* store = context.native_handle();
            if (X509_STORE_CTX_get_error_depth(store) != 0)
                return true;
            auto* certificate = X509_STORE_CTX_get_current_cert(store);
            const auto digest = certificateFingerprint(certificate);
            const bool validDates = certificate &&
                                    X509_cmp_current_time(X509_get0_notBefore(certificate)) < 0 &&
                                    X509_cmp_current_time(X509_get0_notAfter(certificate)) > 0;
            // Le pin remplace la confiance CA pour cette identité auto-signée. Au premier
            // contact, vérifier aussi sa signature ; aucune donnée HELLO n'est encore envoyée.
            const bool selfSigned = certificate &&
                                    X509_check_issued(certificate, certificate) == X509_V_OK &&
                                    X509_verify(certificate, X509_get0_pubkey(certificate)) == 1;
            if (!digest || !validDates || (!options.source.certificateSha256 && !selfSigned)) {
                certificateRejected = true;
                return false;
            }
            fingerprint = *digest;
            if (options.source.certificateSha256 &&
                !sameFingerprint(*options.source.certificateSha256, fingerprint)) {
                certificateMismatch = true;
                return false;
            }
            return true;
        });
        if (SSL_set_tlsext_host_name(current->stream().native_handle(),
                                     options.source.host.c_str()) != 1)
            return current->close(NetworkError::TlsConfiguration);
        deadline.expires_after(options.timings.handshakeTimeout);
        deadline.async_wait([this, current](const asio::error_code& error) {
            if (!error && current == channel) {
                resolver.cancel();
                current->close(NetworkError::Timeout);
            }
        });
        resolver.async_resolve(
            options.source.host, std::to_string(options.source.port),
            [this, current](const asio::error_code& error,
                            const asio::ip::tcp::resolver::results_type& endpoints) {
                if (current != channel || stopping)
                    return;
                if (error)
                    return current->close(NetworkError::ConnectFailed);
                asio::async_connect(
                    current->stream().next_layer(), endpoints,
                    [this, current](const asio::error_code& connectError,
                                    const asio::ip::tcp::endpoint&) {
                        if (current != channel || stopping)
                            return;
                        if (connectError)
                            return current->close(NetworkError::ConnectFailed);
                        current->stream().async_handshake(
                            asio::ssl::stream_base::client,
                            [this, current](const asio::error_code& tlsError) {
                                if (current != channel || stopping)
                                    return;
                                if (tlsError)
                                    return current->close(
                                        certificateMismatch   ? NetworkError::CertificateMismatch
                                        : certificateRejected ? NetworkError::CertificateRejected
                                                              : NetworkError::HandshakeFailed);
                                if (fingerprint.empty())
                                    return current->close(NetworkError::CertificateRejected);
                                setState(ConnectionState::Authenticating);
                                {
                                    const std::lock_guard lock(mutex);
                                    state.certificateSha256 = fingerprint;
                                    state.tlsVersion =
                                        SSL_get_version(current->stream().native_handle());
                                }
                                current->send(protocol::Hello{options.protocolVersion,
                                                              options.userName, options.machineName,
                                                              options.source.token});
                                current->read();
                            });
                    });
            });
    }
    void report(transfer::TransferProgress progress) {
        {
            const std::lock_guard lock(mutex);
            recordTransfer(state.transfers, progress);
        }
        NetworkEvent event{EventKind::TransferProgress};
        event.transfer = progress;
        events.push(std::move(event));
        if (progress.direction == transfer::Direction::Sending &&
            (progress.state == transfer::TransferState::Completed ||
             progress.state == transfer::TransferState::Failed))
            pendingFiles.erase(progress.fileId);
        if (progress.state == transfer::TransferState::Completed)
            logger.log(LogLevel::Info, "File transfer completed");
        if (progress.state == transfer::TransferState::Failed)
            logger.log(LogLevel::Warning, transfer::toString(progress.error));
    }
    void offerFile(const transfer::FileRequest& request) {
        if (options.transfers.directory.empty() ||
            (pendingFiles.size() >= 16 && !pendingFiles.contains(request.fileId))) {
            report({request.fileId, transfer::Direction::Sending, transfer::TransferState::Failed,
                    transfer::TransferError::InvalidRequest});
            return;
        }
        if (pendingFiles.contains(request.fileId))
            return;
        pendingFiles[request.fileId] = request;
        if (transfers)
            transfers->sendFile(request);
    }
    void receive(protocol::Message message) {
        if (awaitingHello) {
            if (const auto* error = std::get_if<protocol::HelloErr>(&message))
                return channel->close(error->reason == "bad_version"
                                          ? NetworkError::VersionMismatch
                                          : NetworkError::AuthenticationFailed);
            const auto* hello = std::get_if<protocol::HelloOk>(&message);
            if (!hello)
                return channel->close(NetworkError::ProtocolViolation);
            if (hello->protocolVersion != protocol::protocolVersion)
                return channel->close(NetworkError::VersionMismatch);
            if (!options.source.certificateSha256) {
                const auto saved = persistPin(options.settingsPath, options.source, fingerprint);
                if (saved != NetworkError::None)
                    return channel->close(saved);
                options.source.certificateSha256 = fingerprint;
            }
            awaitingHello = false;
            deadline.cancel();
            channel->send(options.gpu);
            if (!channel)
                return;
            channel->send(options.preferences);
            if (!channel)
                return;
            policy.reset();
            lastPong = steady_clock::now();
            setState(ConnectionState::Ready);
            {
                const std::lock_guard lock(mutex);
                ++state.readyCount;
            }
            if (!options.transfers.directory.empty()) {
                transfers = std::make_shared<detail::TransferLink>(
                    io, transferWorker, channel, options.transfers, fingerprint,
                    [this](transfer::TransferProgress progress) { report(std::move(progress)); });
                for (const auto& [id, request] : pendingFiles)
                    transfers->sendFile(request);
            }
            events.push({EventKind::Ready});
            logger.log(LogLevel::Info, "Source authenticated; GPU_INFO and CLIENT_PREFS sent");
            tick();
            return;
        }
        if (std::holds_alternative<protocol::Ping>(message))
            channel->send(protocol::Pong{});
        else if (std::holds_alternative<protocol::Pong>(message)) {
            lastPong = steady_clock::now();
            const std::lock_guard lock(mutex);
            ++state.pongsReceived;
        } else if (std::holds_alternative<protocol::ClientKick>(message))
            channel->close(NetworkError::Kicked);
        else if (std::holds_alternative<protocol::JobOffer>(message) ||
                 std::holds_alternative<protocol::TaskAssign>(message) ||
                 std::holds_alternative<protocol::TaskCancel>(message) ||
                 std::holds_alternative<protocol::JobPause>(message) ||
                 std::holds_alternative<protocol::JobResume>(message)) {
            NetworkEvent event{EventKind::JobMessage};
            event.message = std::move(message);
            events.push(std::move(event));
        } else if (!transfers || !transfers->handle(std::move(message)))
            channel->close(NetworkError::ProtocolViolation);
    }
    void tick() {
        if (!channel || stopping)
            return;
        const auto current = channel;
        heartbeat.expires_after(options.timings.pingInterval);
        heartbeat.async_wait([this, current](const asio::error_code& error) {
            if (error || current != channel || stopping)
                return;
            if (steady_clock::now() - lastPong >= options.timings.peerTimeout)
                return current->close(NetworkError::Timeout);
            current->send(protocol::Ping{});
            tick();
        });
    }
    void failed(NetworkError error) {
        if (transfers) {
            transfers->stop();
            transfers.reset();
        }
        {
            const std::lock_guard lock(mutex);
            for (auto& item : state.transfers)
                if (item.state == transfer::TransferState::Transferring ||
                    item.state == transfer::TransferState::Preparing)
                    item.state = transfer::TransferState::Suspended;
        }
        channel.reset();
        resolver.cancel();
        deadline.cancel();
        heartbeat.cancel();
        const bool transient = error == NetworkError::ConnectFailed ||
                               error == NetworkError::IoError || error == NetworkError::Timeout ||
                               error == NetworkError::HandshakeFailed;
        logger.log(error == NetworkError::Stopped ? LogLevel::Info : LogLevel::Warning,
                   toString(error));
        events.push({EventKind::Disconnected, error});
        if (stopping || !transient) {
            setState(ConnectionState::Stopped, error);
            if (error != NetworkError::Stopped)
                events.push({EventKind::Error, error});
            return;
        }
        setState(ConnectionState::Reconnecting, error);
        const auto delay = policy.nextDelay();
        {
            const std::lock_guard lock(mutex);
            ++state.retryCount;
        }
        events.push({EventKind::Reconnecting, error, {}, delay});
        retry.expires_after(delay);
        retry.async_wait([this](const asio::error_code& retryError) {
            if (!retryError && !stopping)
                connect();
        });
    }
    void stop() {
        if (!thread.joinable())
            return;
        asio::post(io, [this] {
            stopping = true;
            retry.cancel();
            resolver.cancel();
            deadline.cancel();
            heartbeat.cancel();
            if (channel)
                channel->close(NetworkError::Stopped);
            setState(ConnectionState::Stopped, NetworkError::Stopped);
            work.reset();
        });
        thread.join();
        workers.join();
        // Les workers annulés peuvent avoir posté une dernière complétion après io.run().
        io.restart();
        io.poll();
    }
};

ClientSocket::ClientSocket(Logger& logger) : m_logger(logger) {}
ClientSocket::~ClientSocket() = default;
NetworkError ClientSocket::start(const ClientOptions& options) {
    stop();
    m_impl = std::make_unique<Impl>(m_logger);
    return m_impl->start(options);
}
void ClientSocket::stop() {
    if (m_impl)
        m_impl->stop();
}
void ClientSocket::updatePreferences(const protocol::ClientPrefs& preferences) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), preferences] {
            if (!protocol::ProtocolCodec::encode(preferences)) {
                impl->events.push({EventKind::Error, NetworkError::InvalidOptions});
                return;
            }
            impl->options.preferences = preferences;
            if (impl->channel && !impl->awaitingHello)
                impl->channel->send(protocol::StatusUpdate{preferences});
        });
}
void ClientSocket::sendFile(const transfer::FileRequest& request) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), request] { impl->offerFile(request); });
}
void ClientSocket::sendJobMessage(const protocol::Message& message) {
    if (!m_impl)
        return;
    asio::post(m_impl->io, [impl = m_impl.get(), message] {
        if (!impl->channel || impl->awaitingHello || impl->stopping)
            return;
        if (std::holds_alternative<protocol::JobAccept>(message) ||
            std::holds_alternative<protocol::JobDecline>(message) ||
            std::holds_alternative<protocol::Progress>(message) ||
            std::holds_alternative<protocol::FileErr>(message))
            impl->channel->send(message);
    });
}
void ClientSocket::cancelFile(const std::string& fileId) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), fileId] {
            impl->pendingFiles.erase(fileId);
            if (impl->transfers)
                impl->transfers->cancelFile(fileId);
        });
}
ClientSnapshot ClientSocket::snapshot() const {
    if (!m_impl)
        return {};
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->state;
}
std::vector<NetworkEvent> ClientSocket::drainEvents() {
    return m_impl ? m_impl->events.drain() : std::vector<NetworkEvent>{};
}
} // namespace blendnet::network
