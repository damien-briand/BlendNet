#pragma once

#include "config/AppSettings.h"
#include "protocol/Messages.h"
#include "transfer/TransferTypes.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace blendnet::network {

enum class NetworkError {
    None,
    InvalidOptions,
    IdentityFailed,
    TlsConfiguration,
    ListenFailed,
    ConnectFailed,
    HandshakeFailed,
    CertificateMismatch,
    CertificateRejected,
    AuthenticationFailed,
    VersionMismatch,
    PinPersistenceFailed,
    Timeout,
    ProtocolViolation,
    QueueFull,
    IoError,
    Kicked,
    Stopped
};
std::string_view toString(NetworkError error);
enum class ConnectionState {
    Disconnected,
    Connecting,
    Authenticating,
    Ready,
    Reconnecting,
    Stopped
};
enum class EventKind {
    Listening,
    Ready,
    Disconnected,
    Reconnecting,
    Error,
    PreferencesChanged,
    TransferProgress
};

struct NetworkEvent {
    NetworkEvent(EventKind eventKind, NetworkError eventError = NetworkError::None,
                 std::string id = {},
                 std::chrono::milliseconds delay = std::chrono::milliseconds(0))
        : kind(eventKind), error(eventError), sessionId(std::move(id)), retryDelay(delay) {}
    EventKind kind;
    NetworkError error = NetworkError::None;
    std::string sessionId;
    std::chrono::milliseconds retryDelay{0};
    std::optional<transfer::TransferProgress> transfer;
};

// RNF-05 : aucun callback réseau vers l'UI, consommation de snapshots/événements.
template <typename T> class EventQueue {
  public:
    void push(T event) {
        const std::lock_guard lock(m_mutex);
        // Bornage pour un consommateur momentanément inactif.
        if (m_events.size() == 1024)
            m_events.erase(m_events.begin());
        m_events.push_back(std::move(event));
    }
    std::vector<T> drain() {
        const std::lock_guard lock(m_mutex);
        std::vector<T> result;
        result.swap(m_events);
        return result;
    }

  private:
    std::mutex m_mutex;
    std::vector<T> m_events;
};

struct NetworkTimings {
    std::chrono::milliseconds pingInterval{10000};
    std::chrono::milliseconds peerTimeout{30000};
    std::chrono::milliseconds handshakeTimeout{10000};
    std::chrono::milliseconds retryInitial{1000};
    std::chrono::milliseconds retryMaximum{300000};
    bool valid() const;
};

struct ServerOptions {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 47050; // 0 : port attribué par l'OS (tests).
    std::string serverName = "BlendNet";
    std::filesystem::path certificateFile;
    std::filesystem::path privateKeyFile;
    std::string token;
    std::size_t maxClients = 64;
    NetworkTimings timings;
    transfer::TransferOptions transfers;
};

struct ClientOptions {
    KnownSource source;
    std::filesystem::path settingsPath = AppSettings::defaultPath();
    std::string userName = "cli-user";
    std::string machineName = "simulated-client";
    std::uint32_t protocolVersion = protocol::protocolVersion;
    protocol::GpuInfo gpu{"CPU (simulated)", "other",      0,     0,
                          "linux",           std::nullopt, "CPU", std::nullopt};
    protocol::ClientPrefs preferences{{}, "AVAILABLE", std::nullopt, 1, std::nullopt};
    NetworkTimings timings;
    transfer::TransferOptions transfers;
};

struct ClientInfo {
    std::string id;
    std::string userName;
    std::string machineName;
    protocol::GpuInfo gpu;
    protocol::ClientPrefs preferences;
};
struct ServerSnapshot {
    std::uint16_t port = 0;
    std::vector<ClientInfo> clients;
    std::vector<transfer::TransferProgress> transfers;
};
struct ClientSnapshot {
    ConnectionState state = ConnectionState::Disconnected;
    NetworkError error = NetworkError::None;
    std::string certificateSha256;
    std::string tlsVersion;
    std::size_t readyCount = 0;
    std::size_t pongsReceived = 0;
    std::size_t retryCount = 0;
    std::vector<transfer::TransferProgress> transfers;
};
void recordTransfer(std::vector<transfer::TransferProgress>& records,
                    const transfer::TransferProgress& progress);

class ReconnectPolicy {
  public:
    explicit ReconnectPolicy(std::chrono::milliseconds initial = std::chrono::seconds(1),
                             std::chrono::milliseconds maximum = std::chrono::minutes(5));
    std::chrono::milliseconds nextDelay();
    void reset();

  private:
    std::chrono::milliseconds m_initial;
    std::chrono::milliseconds m_maximum;
    std::chrono::milliseconds m_next;
};

} // namespace blendnet::network
