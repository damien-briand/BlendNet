#include "network/NetworkTypes.h"

#include <algorithm>

namespace blendnet::network {
void recordTransfer(std::vector<transfer::TransferProgress>& records,
                    const transfer::TransferProgress& progress) {
    const auto found = std::find_if(records.begin(), records.end(), [&progress](const auto& old) {
        return old.fileId == progress.fileId && old.direction == progress.direction &&
               old.peerId == progress.peerId;
    });
    if (found != records.end())
        *found = progress;
    else {
        if (records.size() >= 128)
            records.erase(records.begin());
        records.push_back(progress);
    }
}
std::string_view toString(NetworkError error) {
    switch (error) {
    case NetworkError::None:
        return "ok";
    case NetworkError::InvalidOptions:
        return "invalid network options";
    case NetworkError::IdentityFailed:
        return "cannot create or load server identity";
    case NetworkError::TlsConfiguration:
        return "TLS configuration failed";
    case NetworkError::ListenFailed:
        return "cannot listen on address";
    case NetworkError::ConnectFailed:
        return "cannot connect to source";
    case NetworkError::HandshakeFailed:
        return "TLS handshake failed";
    case NetworkError::CertificateMismatch:
        return "server certificate fingerprint changed";
    case NetworkError::CertificateRejected:
        return "server certificate rejected";
    case NetworkError::AuthenticationFailed:
        return "pairing token rejected";
    case NetworkError::VersionMismatch:
        return "incompatible protocol version";
    case NetworkError::PinPersistenceFailed:
        return "cannot persist server certificate fingerprint";
    case NetworkError::Timeout:
        return "peer timed out";
    case NetworkError::ProtocolViolation:
        return "invalid message or session sequence";
    case NetworkError::QueueFull:
        return "outgoing message queue full";
    case NetworkError::IoError:
        return "TLS connection interrupted";
    case NetworkError::Kicked:
        return "disconnected by source";
    case NetworkError::Stopped:
        return "stopped";
    }
    return "unknown network error";
}
bool NetworkTimings::valid() const {
    return pingInterval.count() > 0 && peerTimeout > pingInterval && handshakeTimeout.count() > 0 &&
           retryInitial.count() > 0 && retryMaximum >= retryInitial;
}
ReconnectPolicy::ReconnectPolicy(std::chrono::milliseconds initial,
                                 std::chrono::milliseconds maximum)
    : m_initial(std::max(initial, std::chrono::milliseconds(1))),
      m_maximum(std::max(maximum, m_initial)), m_next(m_initial) {}
std::chrono::milliseconds ReconnectPolicy::nextDelay() {
    const auto result = m_next;
    m_next = m_next > m_maximum / 2 ? m_maximum : std::min(m_maximum, m_next * 2);
    return result;
}
void ReconnectPolicy::reset() { m_next = m_initial; }
} // namespace blendnet::network
