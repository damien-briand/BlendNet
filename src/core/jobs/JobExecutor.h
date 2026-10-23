#pragma once

#include "archive/ZipService.h"
#include "blender/BlenderInstaller.h"
#include "network/NetworkTypes.h"

namespace blendnet::jobs {
struct ExecutorOptions {
    // The caller dedicates this directory to one source; each executor creates a private run.
    std::filesystem::path directory, blenderCache;
    std::filesystem::path quotaDirectory;
    std::optional<std::uint64_t> quotaBytes;
    std::string backend = "CPU";
    unsigned int threads = 2;
    std::chrono::milliseconds renderTimeout{3600000};
    bool keepCompletedJobs = false;
    archive::ZipLimits zipLimits;
};
enum class ExecutorState {
    Accepted,
    Extracting,
    Installing,
    Rendering,
    Paused,
    Uploading,
    Completed,
    Failed,
    Cancelled
};
std::string_view toString(ExecutorState state);
struct ExecutorEvent {
    ExecutorState state;
    std::string jobId, chunkId, detail;
    std::filesystem::path directory;
};
// RF-40..44. Event-driven owner API; all extraction, installation, rendering and ZIP I/O
// run on a worker. The send callbacks must enqueue work and remain valid until destruction.
class JobExecutor {
  public:
    struct Callbacks {
        std::function<void(const protocol::Message&)> send;
        std::function<void(const transfer::FileRequest&)> sendFile;
        std::function<void(const std::string&)> cancelFile;
    };
    JobExecutor(ExecutorOptions options, Callbacks callbacks,
                blender::Download download = blender::downloadOfficial);
    // Stop the transport before destruction; cancellation joins the worker and then releases
    // workspaces and completed transfer checkpoints (unless retention is enabled).
    ~JobExecutor();
    void handle(const network::NetworkEvent& event);
    std::vector<ExecutorEvent> drainEvents();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace blendnet::jobs
