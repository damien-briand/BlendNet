#pragma once

#include "network/ClientSocket.h"

namespace blendnet {
class SourceConnection {
  public:
    explicit SourceConnection(Logger& logger);
    network::NetworkError start(const network::ClientOptions& options) {
        return m_client.start(options);
    }
    void stop() { m_client.stop(); }
    void updatePreferences(const protocol::ClientPrefs& preferences) {
        m_client.updatePreferences(preferences);
    }
    network::ClientSnapshot snapshot() const { return m_client.snapshot(); }
    std::vector<network::NetworkEvent> drainEvents() { return m_client.drainEvents(); }

  private:
    network::ClientSocket m_client;
};
} // namespace blendnet
