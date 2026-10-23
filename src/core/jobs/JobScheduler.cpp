#include "jobs/JobScheduler.h"
#include "protocol/ProtocolCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace blendnet::jobs {
namespace {
bool terminal(Job::State state) {
    return state == Job::State::Done || state == Job::State::Cancelled;
}
bool active(Chunk::State state) {
    return state == Chunk::State::Assigned || state == Chunk::State::Downloading ||
           state == Chunk::State::Rendering || state == Chunk::State::Uploading;
}
} // namespace
bool JobScheduler::publish(Job job) {
    const FrameRange range{job.frameStart, job.frameEnd, job.frameStep};
    protocol::JobOffer offer{job.id,         job.projectName,    job.engine,
                             job.frameStart, job.frameEnd,       job.frameStep,
                             job.zipBytes,   job.blenderVersion, job.outputFormat};
    if (m_jobs.size() >= 128 || m_jobs.contains(job.id) || job.id.size() > 128 ||
        !protocol::ProtocolCodec::encode(offer) || !range.count() ||
        range.count() > std::uint64_t(std::numeric_limits<int>::max()) || job.chunkSize < 1 ||
        job.chunkSize > 100000 || job.minimumVramMB < 0 ||
        (range.count() - 1) / job.chunkSize + 1 > 100000 || job.outputDirectory.empty())
        return false;
    Record record;
    job.state = Job::State::Published;
    record.view.job = std::move(job);
    record.view.totalFrames = range.count();
    for (std::uint64_t first = 0; first < range.count(); first += record.view.job.chunkSize) {
        const auto last = std::min(range.count() - 1, first + record.view.job.chunkSize - 1);
        Chunk chunk;
        chunk.id = record.view.job.id + "_c" + std::to_string(record.view.chunks.size());
        chunk.frameStart = int(std::int64_t(range.start) + std::int64_t(first) * range.step);
        chunk.frameEnd = int(std::int64_t(range.start) + std::int64_t(last) * range.step);
        chunk.frameStep = range.step;
        record.view.chunks.push_back(std::move(chunk));
    }
    record.view.job.state = Job::State::Recruiting;
    m_jobs.emplace(record.view.job.id, std::move(record));
    return true;
}
Chunk* JobScheduler::assigned(Record& record, std::string_view assignment,
                              std::string_view client) {
    if (terminal(record.view.job.state))
        return nullptr;
    for (auto& chunk : record.view.chunks)
        if (chunk.assignmentId == assignment && chunk.assignedClientId == client &&
            active(chunk.state))
            return &chunk;
    return nullptr;
}
bool JobScheduler::assign(std::string_view jobId, std::size_t index, const std::string& client) {
    auto found = m_jobs.find(jobId);
    if (found == m_jobs.end() || terminal(found->second.view.job.state) ||
        found->second.view.job.state == Job::State::Paused ||
        index >= found->second.view.chunks.size() || client.empty())
        return false;
    auto& record = found->second;
    auto& chunk = record.view.chunks[index];
    if (chunk.state != Chunk::State::Pending)
        return false;
    // Un seul chunk actif par client, y compris entre jobs : pas de surallocation.
    for (const auto& [id, r] : m_jobs)
        for (const auto& c : r.view.chunks)
            if (active(c.state) && c.assignedClientId == client)
                return false;
    chunk.assignedClientId = client;
    chunk.assignmentId = chunk.id + "_a" + std::to_string(chunk.attempts);
    chunk.state = Chunk::State::Downloading;
    record.view.job.state = Job::State::Rendering;
    return true;
}
bool JobScheduler::start(std::string_view id, std::string_view assignment,
                         std::string_view client) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end())
        return false;
    auto* chunk = assigned(found->second, assignment, client);
    if (!chunk || found->second.view.job.state == Job::State::Paused ||
        chunk->state != Chunk::State::Downloading)
        return false;
    chunk->state = Chunk::State::Assigned;
    return true;
}
bool JobScheduler::progress(const protocol::Progress& progress, std::string_view client) {
    auto found = m_jobs.find(progress.jobId);
    if (found == m_jobs.end())
        return false;
    auto* chunk = assigned(found->second, progress.chunkId, client);
    if (!chunk || chunk->state == Chunk::State::Downloading ||
        !protocol::ProtocolCodec::encode(progress))
        return false;
    const FrameRange range{chunk->frameStart, chunk->frameEnd, chunk->frameStep};
    if (progress.framesTotal != std::int64_t(range.count()) ||
        progress.framesDone < chunk->framesDone || progress.currentFrame < range.start ||
        progress.currentFrame > range.end ||
        (std::int64_t(progress.currentFrame) - range.start) % range.step != 0 ||
        std::abs(progress.percent - 100.0 * progress.framesDone / progress.framesTotal) > 0.01)
        return false;
    if (progress.framesDone == chunk->framesDone && chunk->state != Chunk::State::Assigned)
        return false; // Une répétition ne maintient pas indéfiniment une tâche bloquée.
    chunk->framesDone = progress.framesDone;
    chunk->state = progress.framesDone == progress.framesTotal ? Chunk::State::Uploading
                                                               : Chunk::State::Rendering;
    return true;
}
bool JobScheduler::complete(std::string_view id, std::string_view assignment,
                            std::string_view client, std::vector<ValidatedFrame> frames) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end())
        return false;
    auto& record = found->second;
    auto* chunk = assigned(record, assignment, client);
    if (!chunk || chunk->state == Chunk::State::Downloading)
        return false;
    const FrameRange range{chunk->frameStart, chunk->frameEnd, chunk->frameStep};
    std::map<int, ValidatedFrame> checked;
    std::uint32_t width = 0, height = 0;
    if (!record.frames.empty()) {
        width = record.frames.begin()->second.width;
        height = record.frames.begin()->second.height;
    }
    for (auto& frame : frames) {
        if (!width) {
            width = frame.width;
            height = frame.height;
        }
        if (frame.frame < range.start || frame.frame > range.end ||
            (std::int64_t(frame.frame) - range.start) % range.step || frame.path.empty() ||
            !frame.width || !frame.height || frame.width != width || frame.height != height ||
            !checked.emplace(frame.frame, std::move(frame)).second)
            return false;
    }
    if (checked.size() != range.count())
        return false;
    record.frames.merge(checked);
    chunk->state = Chunk::State::Done;
    chunk->framesDone = int(range.count());
    refresh(record);
    return true;
}
void JobScheduler::refresh(Record& record) {
    record.view.validFrames = record.frames.size();
    record.view.failures.clear();
    bool finished = true;
    for (const auto& c : record.view.chunks) {
        if (c.state == Chunk::State::Failed)
            record.view.failures.push_back(c.id);
        if (c.state != Chunk::State::Done)
            finished = false;
    }
    if (finished && record.view.job.state != Job::State::Paused)
        record.view.job.state = Job::State::Assembling;
}
bool JobScheduler::fail(std::string_view id, std::string_view assignment, std::string_view client) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end())
        return false;
    auto* chunk = assigned(found->second, assignment, client);
    if (!chunk)
        return false;
    chunk->state = ++chunk->attempts >= 3 ? Chunk::State::Failed : Chunk::State::Pending;
    chunk->assignedClientId.clear();
    chunk->framesDone = 0;
    refresh(found->second);
    return true;
}
bool JobScheduler::pause(std::string_view id) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || terminal(found->second.view.job.state) ||
        found->second.view.job.state == Job::State::Paused ||
        found->second.view.job.state == Job::State::Assembling)
        return false;
    found->second.view.job.state = Job::State::Paused;
    return true;
}
bool JobScheduler::resume(std::string_view id) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->second.view.job.state != Job::State::Paused)
        return false;
    found->second.view.job.state = Job::State::Rendering;
    refresh(found->second);
    return true;
}
bool JobScheduler::cancel(std::string_view id) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || terminal(found->second.view.job.state))
        return false;
    found->second.view.job.state = Job::State::Cancelled;
    for (auto& chunk : found->second.view.chunks)
        if (active(chunk.state)) {
            chunk.state = Chunk::State::Failed;
            chunk.assignedClientId.clear();
        }
    return true;
}
bool JobScheduler::assembled(std::string_view id, bool valid) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || found->second.view.job.state != Job::State::Assembling)
        return false;
    auto& record = found->second;
    if (valid &&
        (record.frames.size() == record.view.totalFrames || record.view.finalizedWithFailures))
        record.view.job.state = Job::State::Done;
    else {
        // Les fichiers peuvent disparaître après validation : réattribuer, ne jamais annoncer Done.
        record.frames.clear();
        record.view.finalizedWithFailures = false;
        for (auto& chunk : record.view.chunks) {
            chunk.state = ++chunk.attempts >= 3 ? Chunk::State::Failed : Chunk::State::Pending;
            chunk.assignedClientId.clear();
            chunk.framesDone = 0;
        }
        record.view.job.state = Job::State::Rendering;
        refresh(record);
    }
    return true;
}
bool JobScheduler::finalizeWithFailures(std::string_view id) {
    auto found = m_jobs.find(id);
    if (found == m_jobs.end() || terminal(found->second.view.job.state))
        return false;
    auto& view = found->second.view;
    if (view.failures.empty() ||
        std::any_of(view.chunks.begin(), view.chunks.end(), [](const auto& c) {
            return c.state != Chunk::State::Done && c.state != Chunk::State::Failed;
        }))
        return false;
    view.finalizedWithFailures = true;
    view.job.state = Job::State::Assembling;
    return true;
}
std::vector<JobSnapshot> JobScheduler::snapshot() const {
    std::vector<JobSnapshot> result;
    for (const auto& [id, record] : m_jobs)
        result.push_back(record.view);
    return result;
}
const JobSnapshot* JobScheduler::find(std::string_view id) const {
    const auto found = m_jobs.find(id);
    return found == m_jobs.end() ? nullptr : &found->second.view;
}
const std::map<int, ValidatedFrame>& JobScheduler::frames(std::string_view id) const {
    return m_jobs.at(std::string(id)).frames;
}
} // namespace blendnet::jobs
