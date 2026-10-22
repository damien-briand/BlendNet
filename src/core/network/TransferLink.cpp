#include "network/TransferLink.h"

#include <algorithm>

namespace blendnet::network::detail {
using namespace transfer;
namespace {
TransferError fromReason(const std::string& reason) {
    if (reason == "sha_mismatch")
        return TransferError::ChecksumMismatch;
    if (reason == "disk_full")
        return TransferError::DiskFull;
    return TransferError::Aborted;
}
std::string reason(TransferError error) {
    if (error == TransferError::ChecksumMismatch)
        return "sha_mismatch";
    if (error == TransferError::DiskFull)
        return "disk_full";
    return "aborted";
}
} // namespace
TransferLink::TransferLink(asio::io_context& io,
                           asio::strand<asio::thread_pool::executor_type> worker,
                           std::shared_ptr<TlsChannel> channel, TransferOptions options,
                           std::string peerScope, std::function<void(TransferProgress)> onProgress)
    : m_io(io), m_worker(std::move(worker)), m_channel(std::move(channel)),
      m_receiver(std::move(options), std::move(peerScope)), m_onProgress(std::move(onProgress)) {}
void TransferLink::publish(const TransferProgress& progress) {
    if (!m_stopped && m_onProgress)
        m_onProgress(progress);
}
void TransferLink::sendFile(FileRequest request) {
    if (m_stopped)
        return;
    if (std::find(m_requests.begin(), m_requests.end(), request) != m_requests.end())
        return;
    if (m_requests.size() >= 16) {
        publish({request.fileId, Direction::Sending, TransferState::Failed, TransferError::Busy});
        return;
    }
    m_requests.push_back(std::move(request));
    beginNext();
}
void TransferLink::beginNext() {
    if (m_stopped || m_sending || m_requests.empty())
        return;
    m_sending = true;
    const auto request = m_requests.front();
    publish({request.fileId, Direction::Sending, TransferState::Preparing});
    asio::post(m_worker, [self = shared_from_this(), request] {
        if (self->m_stopped)
            return;
        const auto offer = self->m_sender.prepare(request, &self->m_stopped);
        const auto progress = self->m_sender.progress();
        if (self->m_stopped)
            return;
        asio::post(self->m_io, [self, offer, progress] {
            if (self->m_stopped)
                return;
            self->publish(progress);
            if (offer)
                self->m_channel->send(*offer.value);
            else
                self->completeSend(progress);
        });
    });
}
void TransferLink::completeSend(const TransferProgress& progress) {
    if (progress.state != TransferState::Completed && progress.state != TransferState::Failed)
        return;
    m_sending = false;
    m_pumping = false;
    if (!m_requests.empty())
        m_requests.pop_front();
    beginNext();
}
void TransferLink::pump() {
    if (m_stopped || !m_sending || m_pumping)
        return;
    m_pumping = true;
    asio::post(m_worker, [self = shared_from_this()] {
        if (self->m_stopped)
            return;
        auto message = self->m_sender.next();
        const auto progress = self->m_sender.progress();
        if (self->m_stopped)
            return;
        asio::post(self->m_io, [self, message = std::move(message), progress] {
            if (self->m_stopped)
                return;
            if (message) {
                self->m_channel->send(*message, [self] {
                    self->m_pumping = false;
                    self->pump();
                });
            } else {
                self->m_pumping = false;
                if (progress.state == TransferState::Failed) {
                    self->m_channel->send(
                        protocol::FileErr{progress.fileId, reason(progress.error)});
                    self->publish(progress);
                    self->completeSend(progress);
                }
            }
        });
    });
}
bool TransferLink::handle(protocol::Message message) {
    if (!isFileMessage(message))
        return false;
    if (m_stopped)
        return true;
    ++m_pending;
    if (m_pending >= windowBlocks)
        m_channel->pauseReading();
    asio::post(m_worker, [self = shared_from_this(), message = std::move(message)] {
        if (self->m_stopped)
            return;
        std::optional<protocol::Message> reply;
        TransferProgress progress;
        bool sending = false;
        if (const auto* ack = std::get_if<protocol::FileAck>(&message)) {
            sending = true;
            const auto error = self->m_sender.acknowledge(*ack);
            progress = self->m_sender.progress();
            if (error != TransferError::None)
                reply = protocol::FileErr{ack->fileId, reason(error)};
        } else if (const auto* error = std::get_if<protocol::FileErr>(&message);
                   error && self->m_sender.progress().fileId == error->fileId) {
            sending = true;
            self->m_sender.fail(fromReason(error->reason));
            progress = self->m_sender.progress();
        } else {
            reply = self->m_receiver.receive(message, &self->m_stopped);
            progress = self->m_receiver.progress();
            if (std::holds_alternative<protocol::FileBegin>(message) &&
                progress.state == TransferState::Completed)
                progress.state = TransferState::Transferring; // Attendre le FILE_END idempotent.
            if (reply && std::holds_alternative<protocol::FileErr>(*reply)) {
                const auto& error = std::get<protocol::FileErr>(*reply);
                progress.fileId = error.fileId;
                progress.state = TransferState::Failed;
                progress.error = fromReason(error.reason);
            }
        }
        if (self->m_stopped)
            return;
        asio::post(self->m_io, [self, reply = std::move(reply), progress, sending] {
            if (self->m_stopped)
                return;
            --self->m_pending;
            if (reply && !sending && progress.state == TransferState::Completed) {
                self->m_channel->send(*reply, [self, progress] { self->publish(progress); });
            } else {
                if (reply)
                    self->m_channel->send(*reply);
                self->publish(progress);
            }
            if (sending) {
                self->completeSend(progress);
                self->pump();
            }
            if (self->m_pending < windowBlocks)
                self->m_channel->resumeReading();
        });
    });
    return true;
}
void TransferLink::stop() {
    if (m_stopped.exchange(true))
        return;
    // Toujours après les blocs déjà en traitement ; checkpoint et libération du lease.
    asio::post(m_worker, [self = shared_from_this()] { self->m_receiver.suspend(); });
}
} // namespace blendnet::network::detail
