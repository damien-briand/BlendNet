#pragma once

#include "network/NetworkTypes.h"

#include <memory>

namespace blendnet::network {
// start/stop appartiennent au thread propriétaire. snapshot/drainEvents/disconnect sont
// utilisables depuis l'UI ; les opérations de socket sont postées sur le thread réseau.
class TlsServer {
  public:
    explicit TlsServer(Logger& logger);
    ~TlsServer();
    TlsServer(const TlsServer&) = delete;
    TlsServer& operator=(const TlsServer&) = delete;
    NetworkError start(const ServerOptions& options);
    void stop();
    void disconnect(const std::string& sessionId);
    void sendFile(const std::string& sessionId, const transfer::FileRequest& request);
    // Coupure de transport (diagnostic) ; contrairement au kick, le client peut se reconnecter.
    void interrupt(const std::string& sessionId);
    void publishJob(const jobs::Job& job);
    void pauseJob(const std::string& jobId);
    void resumeJob(const std::string& jobId);
    void cancelJob(const std::string& jobId);
    void cancelTask(const std::string& sessionId);
    void finalizeJobWithFailures(const std::string& jobId);
    ServerSnapshot snapshot() const;
    std::vector<NetworkEvent> drainEvents();

  private:
    struct Impl;
    Logger& m_logger;
    std::unique_ptr<Impl> m_impl;
};
} // namespace blendnet::network
