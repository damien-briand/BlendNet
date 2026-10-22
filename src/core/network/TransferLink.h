#pragma once

#include "network/TlsChannel.h"
#include "transfer/TransferEngine.h"

namespace blendnet::network::detail {
// Pont réseau/worker : état disque exclusivement sur un strand du pool, notifications
// exclusivement sur io_context. Un seul fichier sortant et un seul entrant par session.
class TransferLink : public std::enable_shared_from_this<TransferLink> {
  public:
    TransferLink(asio::io_context& io, asio::strand<asio::thread_pool::executor_type> worker,
                 std::shared_ptr<TlsChannel> channel, transfer::TransferOptions options,
                 std::string peerScope, std::function<void(transfer::TransferProgress)> onProgress);
    bool handle(protocol::Message message);
    void sendFile(transfer::FileRequest request);
    void stop();

  private:
    void beginNext();
    void pump();
    void publish(const transfer::TransferProgress& progress);
    void completeSend(const transfer::TransferProgress& progress);
    asio::io_context& m_io;
    asio::strand<asio::thread_pool::executor_type> m_worker;
    std::shared_ptr<TlsChannel> m_channel;
    transfer::TransferReceiver m_receiver;
    transfer::TransferSender m_sender;
    std::function<void(transfer::TransferProgress)> m_onProgress;
    std::atomic_bool m_stopped{false};
    std::deque<transfer::FileRequest> m_requests;
    std::size_t m_pending = 0;
    bool m_sending = false;
    bool m_pumping = false;
};
} // namespace blendnet::network::detail
