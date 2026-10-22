#pragma once

#include "network/NetworkTypes.h"
#include "protocol/ProtocolCodec.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>

namespace blendnet::network::detail {
// Toutes les méthodes sont appelées uniquement sur le thread io_context propriétaire.
class TlsChannel : public std::enable_shared_from_this<TlsChannel> {
  public:
    using Stream = asio::ssl::stream<asio::ip::tcp::socket>;
    TlsChannel(asio::io_context& context, asio::ssl::context& tls) : m_stream(context, tls) {}
    Stream& stream() { return m_stream; }
    void callbacks(std::function<void(protocol::Message)> onMessage,
                   std::function<void(NetworkError)> onClosed);
    void read();
    void send(const protocol::Message& message, std::function<void()> onWritten = {});
    void pauseReading() { m_paused = true; }
    void resumeReading() {
        m_paused = false;
        read();
    }
    void sendAndClose(const protocol::Message& message, NetworkError reason);
    void close(NetworkError reason);

  private:
    void write();
    void enqueue(const protocol::Message& message, std::function<void()> onWritten = {});
    Stream m_stream;
    std::array<std::uint8_t, protocol::headerBytes> m_header{};
    protocol::Bytes m_payload;
    struct PendingWrite {
        std::shared_ptr<protocol::Bytes> bytes;
        std::function<void()> onWritten;
    };
    std::deque<PendingWrite> m_writes;
    std::size_t m_queuedBytes = 0;
    bool m_closed = false;
    bool m_draining = false;
    bool m_paused = false;
    bool m_reading = false;
    NetworkError m_closeReason = NetworkError::Stopped;
    std::function<void(protocol::Message)> m_onMessage;
    std::function<void(NetworkError)> m_onClosed;
};
bool configureTls(asio::ssl::context& context);
} // namespace blendnet::network::detail
