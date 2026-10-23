#pragma once

#include "jobs/Job.h"

namespace blendnet::jobs {
// RF-50/52/54, RNF-03 : propriétaire unique (thread d'orchestration), aucun I/O.
class JobScheduler {
  public:
    bool publish(Job job);
    bool assign(std::string_view jobId, std::size_t chunk, const std::string& client);
    bool start(std::string_view jobId, std::string_view assignment, std::string_view client);
    bool progress(const protocol::Progress& progress, std::string_view client);
    bool complete(std::string_view jobId, std::string_view assignment, std::string_view client,
                  std::vector<ValidatedFrame> frames);
    bool fail(std::string_view jobId, std::string_view assignment, std::string_view client);
    bool pause(std::string_view jobId);
    bool resume(std::string_view jobId);
    bool cancel(std::string_view jobId);
    bool assembled(std::string_view jobId, bool valid);
    bool finalizeWithFailures(std::string_view jobId);
    std::vector<JobSnapshot> snapshot() const;
    const JobSnapshot* find(std::string_view jobId) const;
    const std::map<int, ValidatedFrame>& frames(std::string_view jobId) const;

  private:
    struct Record {
        JobSnapshot view;
        std::map<int, ValidatedFrame> frames;
    };
    std::map<std::string, Record, std::less<>> m_jobs;
    Chunk* assigned(Record& job, std::string_view assignment, std::string_view client);
    void refresh(Record& record);
};
} // namespace blendnet::jobs
