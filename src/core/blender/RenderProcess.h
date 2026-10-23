#pragma once

#include "blender/BlenderTypes.h"

#include <mutex>
#include <set>
#include <thread>

namespace blendnet::blender {
class RenderProgressParser {
  public:
    RenderProgressParser(protocol::TaskAssign task, std::filesystem::path output);
    std::optional<protocol::Progress> consume(std::string_view line);

  private:
    protocol::TaskAssign m_task;
    std::filesystem::path m_output;
    std::set<int> m_completed;
    int m_current;
    std::chrono::steady_clock::time_point m_start{std::chrono::steady_clock::now()};
};
enum class RenderState { Idle, Validating, Rendering, Completed, Failed, Cancelled };
struct RenderSnapshot {
    RenderState state = RenderState::Idle;
    BlenderError error = BlenderError::None;
    std::optional<protocol::Progress> progress{};
    std::chrono::milliseconds elapsed{0};
    std::string diagnostic{};
};
class RenderProcess {
  public:
    ~RenderProcess();
    // Cycle de vie sur le thread propriétaire ; aucune attente dans cancel().
    bool start(const VerifiedBlender& binary, const RenderRequest& request);
    void cancel();
    void wait(); // CLI/tests ; ne pas appeler depuis la boucle UI.
    RenderSnapshot snapshot() const;
    std::vector<RenderSnapshot> drainEvents();
    static BlenderResult<RenderResult>
    run(const VerifiedBlender& binary, const RenderRequest& request,
        const std::atomic_bool* cancelled = nullptr,
        std::function<void(const protocol::Progress&)> progress = {});

  private:
    std::atomic_bool m_cancelled{false}, m_running{false};
    mutable std::mutex m_mutex;
    RenderSnapshot m_snapshot;
    std::vector<RenderSnapshot> m_events;
    std::jthread m_thread;
    void publish(RenderSnapshot state);
};
} // namespace blendnet::blender
