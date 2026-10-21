#include "network/TlsChannel.h"

namespace blendnet::network::detail {
bool configureTls(asio::ssl::context& context) {
    if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1)
        return false;
    context.set_options(asio::ssl::context::no_compression);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_RENEGOTIATION);
#endif
    return true;
}
void TlsChannel::callbacks(std::function<void(protocol::Message)> onMessage,
                           std::function<void(NetworkError)> onClosed) {
    m_onMessage = std::move(onMessage);
    m_onClosed = std::move(onClosed);
}
void TlsChannel::read() {
    if (m_closed || m_draining)
        return;
    asio::async_read(
        m_stream, asio::buffer(m_header),
        [self = shared_from_this()](const asio::error_code& error, std::size_t) {
            if (self->m_closed)
                return;
            if (error)
                return self->close(NetworkError::IoError);
            const auto header = protocol::ProtocolCodec::decodeHeader(self->m_header);
            if (!header)
                return self->close(NetworkError::ProtocolViolation);
            // La taille vient uniquement du codec P2 et a déjà été bornée.
            self->m_payload.resize(header.value->payloadBytes);
            asio::async_read(
                self->m_stream, asio::buffer(self->m_payload),
                [self, header = *header.value](const asio::error_code& bodyError, std::size_t) {
                    if (self->m_closed)
                        return;
                    if (bodyError)
                        return self->close(NetworkError::IoError);
                    auto message = protocol::ProtocolCodec::decodePayload(header, self->m_payload);
                    if (!message)
                        return self->close(NetworkError::ProtocolViolation);
                    if (self->m_onMessage)
                        self->m_onMessage(std::move(*message.value));
                    self->read();
                });
        });
}
void TlsChannel::enqueue(const protocol::Message& message) {
    const auto encoded = protocol::ProtocolCodec::encode(message);
    if (!encoded)
        return close(NetworkError::ProtocolViolation);
    if (m_queuedBytes + encoded.value->size() > 4 * 1024 * 1024)
        return close(NetworkError::QueueFull);
    const bool idle = m_writes.empty();
    m_queuedBytes += encoded.value->size();
    m_writes.push_back(std::make_shared<protocol::Bytes>(std::move(*encoded.value)));
    if (idle)
        write();
}
void TlsChannel::send(const protocol::Message& message) {
    if (!m_closed && !m_draining)
        enqueue(message);
}
void TlsChannel::sendAndClose(const protocol::Message& message, NetworkError reason) {
    if (m_closed || m_draining)
        return;
    m_draining = true;
    m_closeReason = reason;
    enqueue(message);
}
void TlsChannel::write() {
    if (m_closed || m_writes.empty())
        return;
    const auto bytes = m_writes.front();
    asio::async_write(
        m_stream, asio::buffer(*bytes),
        [self = shared_from_this(), bytes](const asio::error_code& error, std::size_t) {
            // bytes reste vivant même si close() vide la file avant la fin d'async_write.
            if (self->m_closed)
                return;
            if (error)
                return self->close(NetworkError::IoError);
            self->m_queuedBytes -= bytes->size();
            self->m_writes.pop_front();
            if (!self->m_writes.empty())
                self->write();
            else if (self->m_draining)
                self->close(self->m_closeReason);
        });
}
void TlsChannel::close(NetworkError reason) {
    if (m_closed)
        return;
    m_closed = true;
    asio::error_code ignored;
    m_stream.lowest_layer().cancel(ignored);
    m_stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    m_stream.lowest_layer().close(ignored);
    m_writes.clear();
    m_queuedBytes = 0;
    if (m_onClosed)
        m_onClosed(reason);
}
} // namespace blendnet::network::detail
