#pragma once

#include "network/TlsServer.h"

namespace blendnet {
// Rôle source : façade du réseau headless ; orchestration des jobs en P5.
class RenderServer {
  public:
    explicit RenderServer(Logger& logger);
    network::NetworkError start(const network::ServerOptions& options) {
        return m_server.start(options);
    }
    void stop() { m_server.stop(); }
    void disconnect(const std::string& id) { m_server.disconnect(id); }
    network::ServerSnapshot snapshot() const { return m_server.snapshot(); }
    std::vector<network::NetworkEvent> drainEvents() { return m_server.drainEvents(); }

  private:
    network::TlsServer m_server;
};
} // namespace blendnet
