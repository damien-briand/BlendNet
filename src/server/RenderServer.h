#pragma once

#include "network/TlsServer.h"

namespace blendnet {
// Rôle source : façade headless ; le métier reste dans core/jobs.
class RenderServer {
  public:
    explicit RenderServer(Logger& logger);
    network::NetworkError start(const network::ServerOptions& options) {
        return m_server.start(options);
    }
    void stop() { m_server.stop(); }
    void disconnect(const std::string& id) { m_server.disconnect(id); }
    void interrupt(const std::string& id) { m_server.interrupt(id); }
    void sendFile(const std::string& id, const transfer::FileRequest& request) {
        m_server.sendFile(id, request);
    }
    void publishJob(const jobs::Job& job) { m_server.publishJob(job); }
    void pauseJob(const std::string& id) { m_server.pauseJob(id); }
    void resumeJob(const std::string& id) { m_server.resumeJob(id); }
    void cancelJob(const std::string& id) { m_server.cancelJob(id); }
    void cancelTask(const std::string& id) { m_server.cancelTask(id); }
    void finalizeJobWithFailures(const std::string& id) { m_server.finalizeJobWithFailures(id); }
    network::ServerSnapshot snapshot() const { return m_server.snapshot(); }
    std::vector<network::NetworkEvent> drainEvents() { return m_server.drainEvents(); }

  private:
    network::TlsServer m_server;
};
} // namespace blendnet
