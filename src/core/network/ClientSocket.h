#pragma once

#include "network/NetworkTypes.h"

#include <memory>

namespace blendnet::network {
class ClientSocket {
  public:
    explicit ClientSocket(Logger& logger);
    ~ClientSocket();
    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;
    NetworkError start(const ClientOptions& options);
    void stop();
    void updatePreferences(const protocol::ClientPrefs& preferences);
    ClientSnapshot snapshot() const;
    std::vector<NetworkEvent> drainEvents();

  private:
    struct Impl;
    Logger& m_logger;
    std::unique_ptr<Impl> m_impl;
};
} // namespace blendnet::network
