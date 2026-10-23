#include "jobs/JobOrchestrator.h"

#include <algorithm>

namespace blendnet::jobs {
namespace {
constexpr auto localId = "__blendnet_local_renderer";
bool runnable(Job::State state) {
    return state == Job::State::Recruiting || state == Job::State::Rendering;
}
} // namespace
JobOrchestrator::JobOrchestrator(Callbacks callbacks, JobTimings timings)
    : m_callbacks(std::move(callbacks)), m_timings(timings) {
    if (m_callbacks.local) {
        Peer local;
        local.info.id = localId;
        m_peers.emplace(localId, std::move(local));
    }
}
void JobOrchestrator::changed() {
    if (m_callbacks.changed)
        m_callbacks.changed();
}
bool JobOrchestrator::publish(Job job) {
    if (!m_scheduler.publish(std::move(job)))
        return false;
    tick();
    return true;
}
void JobOrchestrator::connected(const network::ClientInfo& client) {
    Peer peer;
    peer.info = client;
    m_peers[client.id] = std::move(peer);
    tick();
}
void JobOrchestrator::fail(Peer& peer, bool notify) {
    if (!peer.active)
        return;
    const auto active = *peer.active;
    if (notify && peer.info.id != localId)
        m_callbacks.send(peer.info.id, protocol::TaskCancel{active.job, active.task.chunkId});
    m_scheduler.fail(active.job, active.task.chunkId, peer.info.id);
    peer.offers[active.job].decision = Decision::Declined;
    peer.active.reset();
}
void JobOrchestrator::disconnected(const std::string& id) {
    const auto found = m_peers.find(id);
    if (found == m_peers.end())
        return;
    fail(found->second, false);
    m_peers.erase(found);
    tick();
}
void JobOrchestrator::preferences(const network::ClientInfo& client) {
    auto found = m_peers.find(client.id);
    if (found == m_peers.end())
        return;
    found->second.info = client;
    if (found->second.active) {
        const auto* view = m_scheduler.find(found->second.active->job);
        const auto& t = found->second.active->task;
        if (view && !EligibilityFilter::isEligible(client, view->job,
                                                   {t.frameStart, t.frameEnd, t.frameStep}))
            fail(found->second, true);
    }
    tick();
}
bool JobOrchestrator::message(const std::string& id, const protocol::Message& message) {
    auto found = m_peers.find(id);
    if (found == m_peers.end())
        return false;
    auto& peer = found->second;
    if (const auto* progress = std::get_if<protocol::Progress>(&message)) {
        if (m_scheduler.progress(*progress, id) && peer.active)
            peer.active->deadline = Clock::now() + m_timings.taskTimeout;
    } else if (const auto* accept = std::get_if<protocol::JobAccept>(&message)) {
        auto offer = peer.offers.find(accept->jobId);
        if (offer != peer.offers.end() && offer->second.decision == Decision::Waiting &&
            Clock::now() < offer->second.deadline)
            offer->second.decision = Decision::Accepted;
    } else if (const auto* decline = std::get_if<protocol::JobDecline>(&message)) {
        auto offer = peer.offers.find(decline->jobId);
        if (offer != peer.offers.end()) {
            offer->second.decision = Decision::Declined;
            if (peer.active && peer.active->job == decline->jobId)
                fail(peer, true);
        }
    } else if (const auto* error = std::get_if<protocol::FileErr>(&message)) {
        if (peer.active && error->fileId == resultFileId(peer.active->task.chunkId))
            fail(peer, true);
    } else
        return false;
    tick();
    return true;
}
void JobOrchestrator::transferred(const std::string& id,
                                  const transfer::TransferProgress& progress) {
    auto found = m_peers.find(id);
    if (found == m_peers.end() || !found->second.active)
        return;
    auto& peer = found->second;
    const auto active = *peer.active;
    const auto* view = m_scheduler.find(active.job);
    if (!view)
        return;
    if (progress.direction == transfer::Direction::Sending &&
        progress.fileId == active.job + "_PROJECT_ZIP") {
        if (progress.state == transfer::TransferState::Completed)
            peer.projects.insert(active.job);
        else if (progress.state == transfer::TransferState::Failed)
            fail(peer, true);
        else
            peer.active->deadline = Clock::now() + m_timings.taskTimeout;
    } else if (progress.direction == transfer::Direction::Receiving &&
               progress.fileId == resultFileId(active.task.chunkId)) {
        if (progress.state == transfer::TransferState::Completed && !peer.active->verifying) {
            peer.active->verifying = true;
            m_callbacks.verify(view->job, active.task, id, progress.path);
        } else if (progress.state == transfer::TransferState::Failed)
            fail(peer, true);
        else
            peer.active->deadline = Clock::now() + m_timings.taskTimeout;
    }
    tick();
}
void JobOrchestrator::validated(const std::string& job, const std::string& assignment,
                                const std::string& id, std::vector<ValidatedFrame> frames) {
    auto found = m_peers.find(id);
    if (found == m_peers.end() || !found->second.active ||
        found->second.active->task.chunkId != assignment || found->second.active->job != job)
        return;
    if (!m_scheduler.complete(job, assignment, id, std::move(frames)))
        fail(found->second, true);
    else
        found->second.active.reset();
    tick();
}
void JobOrchestrator::assembled(const std::string& job, bool success) {
    m_assembling.erase(job);
    m_scheduler.assembled(job, success);
    tick();
}
void JobOrchestrator::schedule(Clock::time_point now) {
    const auto views = m_scheduler.snapshot();
    for (const auto& view : views) {
        if (view.job.state == Job::State::Assembling) {
            if (m_assembling.insert(view.job.id).second)
                m_callbacks.assemble(view.job, m_scheduler.frames(view.job.id),
                                     view.finalizedWithFailures);
            continue;
        }
        if (!runnable(view.job.state))
            continue;
        for (auto& [id, peer] : m_peers) {
            if (id == localId || peer.offers.contains(view.job.id))
                continue;
            const bool eligible =
                std::any_of(view.chunks.begin(), view.chunks.end(), [&](const auto& c) {
                    return c.state == Chunk::State::Pending &&
                           EligibilityFilter::isEligible(peer.info, view.job,
                                                         {c.frameStart, c.frameEnd, c.frameStep});
                });
            if (!eligible)
                continue;
            peer.offers.emplace(view.job.id,
                                Offer{Decision::Waiting, now + m_timings.offerTimeout});
            m_callbacks.send(id,
                             protocol::JobOffer{view.job.id, view.job.projectName, view.job.engine,
                                                view.job.frameStart, view.job.frameEnd,
                                                view.job.frameStep, view.job.zipBytes,
                                                view.job.blenderVersion, view.job.outputFormat});
        }
        bool remotePossible = false;
        for (auto& [id, peer] : m_peers) {
            if (id == localId)
                continue;
            const auto offer = peer.offers.find(view.job.id);
            if (offer == peer.offers.end() || offer->second.decision == Decision::Declined)
                continue;
            const auto* live = m_scheduler.find(view.job.id);
            const bool eligible =
                std::any_of(live->chunks.begin(), live->chunks.end(), [&](const auto& c) {
                    return c.state == Chunk::State::Pending &&
                           EligibilityFilter::isEligible(peer.info, live->job,
                                                         {c.frameStart, c.frameEnd, c.frameStep});
                });
            if (!eligible && !(peer.active && peer.active->job == view.job.id))
                continue;
            remotePossible = true;
            if (peer.active || offer->second.decision != Decision::Accepted)
                continue;
            for (std::size_t i = 0; i < view.chunks.size(); ++i) {
                const auto* current = m_scheduler.find(view.job.id);
                const auto& chunk = current->chunks[i];
                if (!EligibilityFilter::isEligible(
                        peer.info, current->job,
                        {chunk.frameStart, chunk.frameEnd, chunk.frameStep}) ||
                    !m_scheduler.assign(view.job.id, i, id))
                    continue;
                const auto& assigned = m_scheduler.find(view.job.id)->chunks[i];
                peer.active = Assignment{view.job.id, taskFor(view.job, assigned),
                                         now + m_timings.taskTimeout};
                if (!peer.projects.contains(view.job.id))
                    m_callbacks.sendFile(
                        id, {view.job.projectZip, view.job.id + "_PROJECT_ZIP", "PROJECT_ZIP"});
                break;
            }
        }
        if (!remotePossible && m_callbacks.local) {
            auto& peer = m_peers.at(localId);
            if (!peer.active)
                for (std::size_t i = 0; i < view.chunks.size(); ++i) {
                    if (!m_scheduler.assign(view.job.id, i, localId))
                        continue;
                    const auto& chunk = m_scheduler.find(view.job.id)->chunks[i];
                    peer.active = Assignment{view.job.id, taskFor(view.job, chunk),
                                             now + m_timings.taskTimeout, true};
                    m_scheduler.start(view.job.id, chunk.assignmentId, localId);
                    m_callbacks.local(view.job, peer.active->task);
                    break;
                }
        }
    }
    for (auto& [id, peer] : m_peers)
        if (id != localId && peer.active && peer.projects.contains(peer.active->job) &&
            m_scheduler.start(peer.active->job, peer.active->task.chunkId, id))
            m_callbacks.send(id, peer.active->task);
}
void JobOrchestrator::tick(Clock::time_point now) {
    if (m_scheduling)
        return;
    m_scheduling = true;
    for (auto& [id, peer] : m_peers) {
        for (auto& [job, offer] : peer.offers)
            if (offer.decision == Decision::Waiting && now >= offer.deadline)
                offer.decision = Decision::Declined;
        if (peer.active) {
            const auto* view = m_scheduler.find(peer.active->job);
            if (view && view->job.state != Job::State::Paused && now >= peer.active->deadline)
                fail(peer, true);
        }
    }
    schedule(now);
    m_scheduling = false;
    changed();
}
void JobOrchestrator::pause(const std::string& job) {
    if (!m_scheduler.pause(job))
        return;
    for (const auto& [id, peer] : m_peers)
        if (id != localId && peer.offers.contains(job))
            m_callbacks.send(id, protocol::JobPause{job});
    changed();
}
void JobOrchestrator::resume(const std::string& job) {
    if (!m_scheduler.resume(job))
        return;
    for (auto& [id, peer] : m_peers) {
        if (id != localId && peer.offers.contains(job))
            m_callbacks.send(id, protocol::JobResume{job});
        if (peer.active && peer.active->job == job)
            peer.active->deadline = Clock::now() + m_timings.taskTimeout;
    }
    tick();
}
void JobOrchestrator::cancel(const std::string& job) {
    if (!m_scheduler.cancel(job))
        return;
    for (auto& [id, peer] : m_peers)
        if (peer.active && peer.active->job == job) {
            if (id != localId)
                m_callbacks.send(id, protocol::TaskCancel{job, peer.active->task.chunkId});
            peer.active.reset();
        }
    tick();
}
void JobOrchestrator::cancelTask(const std::string& client) {
    const auto found = m_peers.find(client);
    if (found == m_peers.end())
        return;
    fail(found->second, true);
    tick();
}
bool JobOrchestrator::finalizeWithFailures(const std::string& job) {
    const bool result = m_scheduler.finalizeWithFailures(job);
    if (result)
        tick();
    return result;
}
} // namespace blendnet::jobs
