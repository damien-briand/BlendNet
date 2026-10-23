#pragma once

#include "network/NetworkTypes.h"

#include <condition_variable>
#include <functional>
#include <set>
#include <thread>

namespace blendnet::jobs {
// RenderProcess mocké de P5, activé explicitement par la CLI et les tests.
class SimulatedWorker {
  public:
    SimulatedWorker(std::filesystem::path directory, std::chrono::milliseconds frameDelay,
                    std::function<void(const protocol::Message&)> send,
                    std::function<void(const transfer::FileRequest&)> sendFile,
                    bool decline = false);
    ~SimulatedWorker();
    void handle(const network::NetworkEvent& event);

  private:
    std::filesystem::path m_directory;
    std::chrono::milliseconds m_delay;
    std::function<void(const protocol::Message&)> m_send;
    std::function<void(const transfer::FileRequest&)> m_sendFile;
    bool m_decline;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stopping = false;
    std::uint64_t m_generation = 0;
    std::optional<protocol::TaskAssign> m_pending, m_current;
    std::set<std::string> m_paused;
    std::jthread m_thread;
    void run();
};
} // namespace blendnet::jobs
