#pragma once

#include "transfer/TransferTypes.h"

#include <atomic>
#include <memory>

namespace blendnet::transfer {
// RNF-02/04/05 : buffers bornés, SHA-256 en flux. À appeler sur un worker, jamais dans l'UI.
TransferResult<std::string> sha256File(const std::filesystem::path& path,
                                       const std::atomic_bool* cancelled = nullptr);
std::string storageKey(std::string_view text);
// Only a completed, unleased P4 checkpoint with the expected fileId may be removed.
// Caller supplies a path obtained from a validated Receiving/Completed event.
bool removeCompletedTransfer(const std::filesystem::path& path, const std::string& fileId);

class TransferReceiver {
  public:
    TransferReceiver(TransferOptions options, std::string peerScope);
    ~TransferReceiver();
    TransferReceiver(const TransferReceiver&) = delete;
    TransferReceiver& operator=(const TransferReceiver&) = delete;
    // Réponse FILE_ACK/FILE_ERR éventuelle. Aucun octet n'est promu avant FILE_END valide.
    std::optional<protocol::Message> receive(const protocol::Message& message,
                                             const std::atomic_bool* cancelled = nullptr);
    void suspend();
    TransferProgress progress() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class TransferSender {
  public:
    TransferSender();
    ~TransferSender();
    TransferSender(const TransferSender&) = delete;
    TransferSender& operator=(const TransferSender&) = delete;
    TransferResult<protocol::FileBegin> prepare(const FileRequest& request,
                                                const std::atomic_bool* cancelled = nullptr);
    TransferError acknowledge(const protocol::FileAck& ack);
    // Vide : fenêtre pleine, attente d'ACK ou transfert terminé.
    std::optional<protocol::Message> next();
    void fail(TransferError error);
    TransferProgress progress() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
bool isFileMessage(const protocol::Message& message);
} // namespace blendnet::transfer
