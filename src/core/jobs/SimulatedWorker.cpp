#include "jobs/SimulatedWorker.h"
#include "jobs/ResultAssembler.h"
#include "transfer/TransferEngine.h"

namespace blendnet::jobs {
SimulatedWorker::SimulatedWorker(std::filesystem::path directory, std::chrono::milliseconds delay,
                                 std::function<void(const protocol::Message&)> send,
                                 std::function<void(const transfer::FileRequest&)> sendFile,
                                 bool decline)
    : m_directory(std::move(directory)), m_delay(delay), m_send(std::move(send)),
      m_sendFile(std::move(sendFile)), m_decline(decline), m_thread([this] { run(); }) {}
SimulatedWorker::~SimulatedWorker() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    m_thread.join();
}
void SimulatedWorker::handle(const network::NetworkEvent& event) {
    if (event.kind == network::EventKind::Reconnecting || event.kind == network::EventKind::Error ||
        event.kind == network::EventKind::Disconnected) {
        const std::lock_guard lock(m_mutex);
        ++m_generation;
        m_pending.reset();
        m_paused.clear();
        m_wake.notify_all();
        return;
    }
    if (event.kind != network::EventKind::JobMessage || !event.message)
        return;
    if (const auto* offer = std::get_if<protocol::JobOffer>(&*event.message)) {
        if (m_decline || offer->outputFormat != "PNG")
            m_send(protocol::JobDecline{offer->jobId});
        else
            m_send(protocol::JobAccept{offer->jobId});
    } else {
        const std::lock_guard lock(m_mutex);
        if (const auto* task = std::get_if<protocol::TaskAssign>(&*event.message)) {
            if (FrameRange{task->frameStart, task->frameEnd, task->frameStep}.count() > 100000)
                return;
            ++m_generation;
            m_pending = *task;
        } else if (const auto* cancel = std::get_if<protocol::TaskCancel>(&*event.message)) {
            if ((m_current && m_current->chunkId == cancel->chunkId) ||
                (m_pending && m_pending->chunkId == cancel->chunkId)) {
                ++m_generation;
                m_pending.reset();
            }
        } else if (const auto* pause = std::get_if<protocol::JobPause>(&*event.message))
            m_paused.insert(pause->jobId);
        else if (const auto* resume = std::get_if<protocol::JobResume>(&*event.message))
            m_paused.erase(resume->jobId);
        m_wake.notify_all();
    }
}
void SimulatedWorker::run() {
    while (true) {
        protocol::TaskAssign task;
        std::uint64_t generation;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || m_pending.has_value(); });
            if (m_stopping)
                return;
            task = *m_pending;
            m_pending.reset();
            m_current = task;
            generation = m_generation;
        }
        const auto total = FrameRange{task.frameStart, task.frameEnd, task.frameStep}.count();
        bool cancelled = false;
        for (std::uint64_t done = 1; done <= total; ++done) {
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] {
                    return m_stopping || generation != m_generation ||
                           !m_paused.contains(task.jobId);
                });
                if (m_stopping)
                    return;
                if (generation != m_generation) {
                    cancelled = true;
                    break;
                }
                const bool interrupted = m_wake.wait_for(lock, m_delay, [&] {
                    return m_stopping || generation != m_generation ||
                           m_paused.contains(task.jobId);
                });
                if (interrupted) {
                    --done;
                    continue;
                }
            }
            m_send(protocol::Progress{
                task.jobId,
                task.chunkId,
                int(std::int64_t(task.frameStart) + std::int64_t(done - 1) * task.frameStep),
                int(done),
                int(total),
                100.0 * done / total,
                {}});
        }
        if (cancelled)
            continue;
        const auto archive = m_directory / transfer::storageKey(task.chunkId) / "result.zip";
        const bool valid = ResultAssembler::simulatedResult(task, archive);
        {
            const std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            if (generation != m_generation)
                continue;
        }
        if (valid)
            m_sendFile({archive, resultFileId(task.chunkId), "RESULT_ZIP"});
        else
            m_send(protocol::FileErr{resultFileId(task.chunkId), "aborted"});
    }
}
} // namespace blendnet::jobs
