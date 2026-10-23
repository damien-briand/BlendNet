#pragma once

#include "jobs/EligibilityFilter.h"
#include "jobs/JobScheduler.h"

#include <functional>
#include <set>

namespace blendnet::jobs {
class JobOrchestrator {
  public:
    using Clock = std::chrono::steady_clock;
    struct Callbacks {
        std::function<void(const std::string&, const protocol::Message&)> send;
        std::function<void(const std::string&, const transfer::FileRequest&)> sendFile;
        std::function<void(const Job&, const protocol::TaskAssign&, const std::string&,
                           const std::filesystem::path&)>
            verify;
        std::function<void(const Job&, const std::map<int, ValidatedFrame>&, bool)> assemble;
        std::function<void(const Job&, const protocol::TaskAssign&)> local;
        std::function<void()> changed;
    };
    explicit JobOrchestrator(Callbacks callbacks, JobTimings timings = {});
    bool publish(Job job);
    void connected(const network::ClientInfo& client);
    void disconnected(const std::string& client);
    void preferences(const network::ClientInfo& client);
    bool message(const std::string& client, const protocol::Message& message);
    void transferred(const std::string& client, const transfer::TransferProgress& progress);
    void validated(const std::string& job, const std::string& assignment, const std::string& client,
                   std::vector<ValidatedFrame> frames);
    void assembled(const std::string& job, bool success);
    void tick(Clock::time_point now = Clock::now());
    void pause(const std::string& job);
    void resume(const std::string& job);
    void cancel(const std::string& job);
    void cancelTask(const std::string& client);
    bool finalizeWithFailures(const std::string& job);
    std::vector<JobSnapshot> snapshot() const { return m_scheduler.snapshot(); }

  private:
    enum class Decision { Waiting, Accepted, Declined };
    struct Offer {
        Decision decision = Decision::Waiting;
        Clock::time_point deadline;
    };
    struct Assignment {
        std::string job;
        protocol::TaskAssign task;
        Clock::time_point deadline;
        bool verifying = false;
    };
    struct Peer {
        network::ClientInfo info;
        std::map<std::string, Offer> offers;
        std::set<std::string> projects;
        std::optional<Assignment> active;
    };
    Callbacks m_callbacks;
    JobTimings m_timings;
    JobScheduler m_scheduler;
    std::map<std::string, Peer> m_peers;
    std::set<std::string> m_assembling;
    bool m_scheduling = false;
    void schedule(Clock::time_point now);
    void fail(Peer& peer, bool notify);
    void changed();
};
} // namespace blendnet::jobs
