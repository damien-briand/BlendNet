#include "jobs/JobExecutor.h"
#include "blender/RenderProcess.h"
#include "network/Security.h"
#include "protocol/ProtocolCodec.h"
#include "transfer/TransferEngine.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <thread>

namespace blendnet::jobs {
std::string_view toString(ExecutorState state) {
    switch (state) {
    case ExecutorState::Accepted:
        return "accepted";
    case ExecutorState::Extracting:
        return "extracting";
    case ExecutorState::Installing:
        return "installing";
    case ExecutorState::Rendering:
        return "rendering";
    case ExecutorState::Paused:
        return "paused";
    case ExecutorState::Uploading:
        return "uploading";
    case ExecutorState::Completed:
        return "completed";
    case ExecutorState::Failed:
        return "failed";
    case ExecutorState::Cancelled:
        return "cancelled";
    }
    return "failed";
}
struct JobExecutor::Impl {
    ExecutorOptions options;
    Callbacks callbacks;
    blender::BlenderInstaller installer;
    std::filesystem::path root;
    network::EventQueue<ExecutorEvent> events;
    struct Queued {
        network::NetworkEvent event;
        std::uint64_t generation;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Queued> queue;
    std::atomic_bool cancelled{false};
    bool stopping = false;
    std::uint64_t generation = 0;
    std::optional<protocol::TaskAssign> active;
    std::optional<protocol::Progress> currentProgress;
    std::set<std::string> paused;
    // Owner-side admission stays responsive even while the worker installs or renders.
    std::map<std::string, protocol::JobOffer> accepted;
    std::map<std::string, protocol::JobOffer> offers;
    std::map<std::string, std::filesystem::path> projects;
    std::map<std::string, std::filesystem::path> receivedProjects;
    std::map<std::string, protocol::TaskAssign> uploads;
    std::optional<Queued> pending;
    std::jthread thread;

    Impl(ExecutorOptions opts, Callbacks cb, blender::Download download)
        : options(std::move(opts)), callbacks(std::move(cb)),
          installer(options.blenderCache, std::move(download)) {
        const auto id = network::randomSessionId();
        if (id && !options.directory.empty())
            root = std::filesystem::absolute(options.directory) / *id;
        thread = std::jthread([this] { run(); });
    }
    ~Impl() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
            cancelled = true;
            if (active && callbacks.cancelFile)
                callbacks.cancelFile(resultFileId(active->chunkId));
        }
        wake.notify_all();
        thread.join();
        // The caller stops its transport before destruction, so no sender still reads these files.
        if (!options.keepCompletedJobs && !root.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
            for (const auto& [id, path] : receivedProjects)
                transfer::removeCompletedTransfer(path, id + "_PROJECT_ZIP");
        }
    }
    std::filesystem::path attempt(const protocol::TaskAssign& task) const {
        // Keep one full digest for the pair, avoiding two long directory names on Windows.
        return root / transfer::storageKey(task.jobId + std::string(1, '\0') + task.chunkId);
    }
    void publish(ExecutorState state, const protocol::TaskAssign& task, std::string detail = {}) {
        events.push({state, task.jobId, task.chunkId, std::move(detail), attempt(task)});
    }
    void fail(const protocol::TaskAssign& task, std::string detail) {
        publish(ExecutorState::Failed, task, std::move(detail));
        callbacks.send(protocol::FileErr{resultFileId(task.chunkId), "aborted"});
    }
    bool live(std::uint64_t epoch) {
        const std::lock_guard lock(mutex);
        return !stopping && generation == epoch;
    }
    bool prepare(std::uint64_t epoch, const protocol::TaskAssign& task) {
        std::unique_lock lock(mutex);
        if (paused.contains(task.jobId))
            publish(ExecutorState::Paused, task);
        wake.wait(lock,
                  [&] { return stopping || generation != epoch || !paused.contains(task.jobId); });
        if (stopping || generation != epoch)
            return false;
        cancelled = false;
        return true;
    }
    std::uint64_t available() {
        if (!options.quotaBytes)
            return options.zipLimits.expandedBytes;
        std::uint64_t used = 0;
        const auto directory =
            options.quotaDirectory.empty() ? options.directory : options.quotaDirectory;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (cancelled)
                return 0;
            if (entry.is_symlink())
                throw std::runtime_error("symlink in workspace");
            if (entry.is_regular_file()) {
                const auto size = entry.file_size();
                if (size >= *options.quotaBytes - used)
                    return 0;
                used += size;
            }
        }
        return std::min(options.zipLimits.expandedBytes, *options.quotaBytes - used);
    }
    void execute(const protocol::TaskAssign& task, std::uint64_t epoch) {
        const auto offer = offers.find(task.jobId);
        const FrameRange range{task.frameStart, task.frameEnd, task.frameStep};
        if (!live(epoch))
            return;
        if (offer == offers.end() || !range.count() || range.count() > 100000 ||
            task.frameStart < offer->second.frameStart || task.frameEnd > offer->second.frameEnd ||
            task.frameStep != offer->second.frameStep ||
            (std::int64_t(task.frameStart) - offer->second.frameStart) % task.frameStep ||
            task.outputFormat != offer->second.outputFormat || task.outputPattern != "frame_####") {
            fail(task, "invalid assignment");
            return;
        }
        const auto directory = attempt(task);
        protocol::Progress highest{
            task.jobId, task.chunkId, task.frameStart, 0, int(range.count()), 0, {}};
        {
            const std::lock_guard lock(mutex);
            currentProgress = highest;
        }
        std::mutex progressMutex;
        std::condition_variable_any heartbeatWake;
        std::jthread heartbeat([&](std::stop_token stop) {
            std::mutex waitMutex;
            std::unique_lock waitLock(waitMutex);
            while (!stop.stop_requested()) {
                heartbeatWake.wait_for(waitLock, stop, std::chrono::seconds(5),
                                       [] { return false; });
                if (stop.stop_requested())
                    return;
                protocol::Progress progress;
                {
                    const std::lock_guard lock(progressMutex);
                    progress = highest;
                }
                if (live(epoch) && !cancelled)
                    callbacks.send(progress);
            }
        });
        for (;;) {
            if (!prepare(epoch, task)) {
                publish(ExecutorState::Cancelled, task);
                return;
            }
            // On resume restart the same lease in a fresh directory. Never reuse partial images.
            std::filesystem::remove_all(directory);
            if (!archive::ZipService::privateDirectory(directory)) {
                fail(task, "cannot create private workspace");
                return;
            }
            auto limits = options.zipLimits;
            limits.expandedBytes = std::min(limits.expandedBytes, available());
            publish(ExecutorState::Extracting, task);
            const auto scene = archive::ZipService::extractProject(
                projects.at(task.jobId), directory / "project", limits, &cancelled);
            if (!scene) {
                if (cancelled)
                    continue;
                fail(task, "project archive rejected or quota exceeded");
                return;
            }
            publish(ExecutorState::Installing, task);
            const auto binary = installer.ensureVersion(offer->second.blenderVersion, &cancelled);
            if (!binary) {
                if (cancelled)
                    continue;
                fail(task, std::string(blender::toString(binary.error)));
                return;
            }
            if (cancelled)
                continue;
            publish(ExecutorState::Rendering, task);
            blender::RenderRequest request{task,
                                           *scene,
                                           directory / "frames",
                                           offer->second.engine,
                                           options.backend,
                                           options.threads,
                                           options.renderTimeout};
            bool quotaExceeded = false;
            const auto rendered = blender::RenderProcess::run(
                *binary.value, request, &cancelled, [&](const protocol::Progress& progress) {
                    protocol::Progress report;
                    {
                        const std::lock_guard lock(progressMutex);
                        if (progress.framesDone >= highest.framesDone)
                            highest = progress;
                        report = highest;
                    }
                    if (live(epoch))
                        callbacks.send(report);
                    {
                        const std::lock_guard lock(mutex);
                        currentProgress = report;
                    }
                    if (options.quotaBytes && !cancelled && !available()) {
                        quotaExceeded = true;
                        cancelled = true;
                    }
                });
            if (cancelled) {
                if (live(epoch) && quotaExceeded) {
                    fail(task, "workspace quota exceeded");
                    return;
                }
                continue;
            }
            if (!live(epoch))
                return;
            const auto result = directory / "result.zip";
            // A failed render can still return inspected frames and a bounded error report.
            const auto error =
                rendered ? std::string{} : std::string(blender::toString(rendered.error));
            if (!archive::ZipService::packResult(task, request.outputDirectory, result, error,
                                                 &cancelled)) {
                if (cancelled)
                    continue;
                fail(task, error.empty() ? "cannot package results" : error);
                return;
            }
            if (options.quotaBytes && !available()) {
                fail(task, "workspace quota exceeded");
                return;
            }
            {
                const std::lock_guard lock(mutex);
                if (stopping || generation != epoch || cancelled)
                    continue;
                uploads[resultFileId(task.chunkId)] = task;
                publish(ExecutorState::Uploading, task, error);
                callbacks.sendFile({result, resultFileId(task.chunkId), "RESULT_ZIP"});
            }
            return;
        }
    }
    void process(const Queued& item) {
        const auto& event = item.event;
        if (event.kind == network::EventKind::Disconnected ||
            event.kind == network::EventKind::Error ||
            event.kind == network::EventKind::Reconnecting) {
            offers.clear();
            projects.clear();
            pending.reset();
            return;
        }
        if (event.message) {
            if (const auto* offer = std::get_if<protocol::JobOffer>(&*event.message)) {
                const auto platform = blender::Platform::current();
                if (!platform || !blender::officialRelease(offer->blenderVersion, *platform) ||
                    root.empty() || !archive::ZipService::privateDirectory(root)) {
                    callbacks.send(protocol::JobDecline{offer->jobId});
                    return;
                }
                const auto existing = offers.find(offer->jobId);
                if (existing != offers.end() && existing->second != *offer) {
                    callbacks.send(protocol::JobDecline{offer->jobId});
                    return;
                }
                offers[offer->jobId] = *offer;
                events.push({ExecutorState::Accepted, offer->jobId, {}, {}, root});
            } else if (std::holds_alternative<protocol::TaskAssign>(*event.message))
                pending = item;
        }
        if (event.transfer) {
            const auto& progress = *event.transfer;
            if (progress.direction == transfer::Direction::Receiving) {
                for (const auto& [id, offer] : offers)
                    if (progress.fileId == id + "_PROJECT_ZIP") {
                        if (progress.state == transfer::TransferState::Completed &&
                            std::filesystem::file_size(progress.path) == offer.zipBytes) {
                            projects[id] = progress.path;
                            receivedProjects[id] = progress.path;
                        } else {
                            callbacks.send(protocol::JobDecline{id});
                            events.push(
                                {ExecutorState::Failed, id, {}, "project transfer failed", root});
                        }
                        break;
                    }
            } else {
                const auto found = uploads.find(progress.fileId);
                if (found != uploads.end()) {
                    const auto task = found->second;
                    if (progress.state == transfer::TransferState::Completed) {
                        if (!options.keepCompletedJobs)
                            std::filesystem::remove_all(attempt(task));
                        publish(ExecutorState::Completed, task);
                        const std::lock_guard lock(mutex);
                        if (active && active->chunkId == task.chunkId &&
                            active->jobId == task.jobId)
                            active.reset();
                    } else
                        fail(task, "result transfer failed");
                    uploads.erase(found);
                }
            }
        }
        if (pending) {
            const auto task = std::get<protocol::TaskAssign>(*pending->event.message);
            if (!live(pending->generation))
                pending.reset();
            else if (projects.contains(task.jobId)) {
                const auto epoch = pending->generation;
                pending.reset();
                execute(task, epoch);
            }
        }
    }
    void run() {
        for (;;) {
            std::optional<Queued> item;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                item = std::move(queue.front());
                queue.pop_front();
            }
            try {
                process(*item);
            } catch (const std::exception&) {
                std::optional<protocol::TaskAssign> task;
                {
                    const std::lock_guard lock(mutex);
                    task = active;
                }
                if (task)
                    fail(*task, "executor filesystem or process error");
            }
        }
    }
};
JobExecutor::JobExecutor(ExecutorOptions options, Callbacks callbacks, blender::Download download)
    : m_impl(
          std::make_unique<Impl>(std::move(options), std::move(callbacks), std::move(download))) {}
JobExecutor::~JobExecutor() = default;
void JobExecutor::handle(const network::NetworkEvent& event) {
    if (event.kind == network::EventKind::TransferProgress &&
        (!event.transfer || (event.transfer->state != transfer::TransferState::Completed &&
                             event.transfer->state != transfer::TransferState::Failed)))
        return;
    if (event.kind != network::EventKind::JobMessage &&
        event.kind != network::EventKind::TransferProgress &&
        event.kind != network::EventKind::Disconnected && event.kind != network::EventKind::Error &&
        event.kind != network::EventKind::Reconnecting)
        return;
    const std::lock_guard lock(m_impl->mutex);
    if (m_impl->stopping)
        return;
    bool invalidate = event.kind == network::EventKind::Disconnected ||
                      event.kind == network::EventKind::Error ||
                      event.kind == network::EventKind::Reconnecting;
    if (event.message) {
        if (const auto* offer = std::get_if<protocol::JobOffer>(&*event.message)) {
            const auto platform = blender::Platform::current();
            const auto existing = m_impl->accepted.find(offer->jobId);
            if (!platform || m_impl->root.empty() ||
                !blender::officialRelease(offer->blenderVersion, *platform) ||
                (existing == m_impl->accepted.end() && m_impl->accepted.size() >= 128) ||
                (existing != m_impl->accepted.end() && existing->second != *offer)) {
                m_impl->callbacks.send(protocol::JobDecline{offer->jobId});
                return;
            }
            m_impl->accepted[offer->jobId] = *offer;
            m_impl->callbacks.send(protocol::JobAccept{offer->jobId});
        }
        if (const auto* task = std::get_if<protocol::TaskAssign>(&*event.message)) {
            if (m_impl->active && *m_impl->active == *task)
                return;
            invalidate = true;
            if (m_impl->active && m_impl->callbacks.cancelFile)
                m_impl->callbacks.cancelFile(resultFileId(m_impl->active->chunkId));
            m_impl->active = *task;
        } else if (const auto* cancel = std::get_if<protocol::TaskCancel>(&*event.message))
            invalidate = m_impl->active && m_impl->active->jobId == cancel->jobId &&
                         m_impl->active->chunkId == cancel->chunkId;
        else if (const auto* pause = std::get_if<protocol::JobPause>(&*event.message)) {
            m_impl->paused.insert(pause->jobId);
            if (m_impl->active && m_impl->active->jobId == pause->jobId)
                m_impl->cancelled = true;
        } else if (const auto* resume = std::get_if<protocol::JobResume>(&*event.message))
            m_impl->paused.erase(resume->jobId);
    }
    if (invalidate) {
        ++m_impl->generation;
        m_impl->cancelled = true;
        if (m_impl->active && m_impl->callbacks.cancelFile &&
            !(event.message && std::holds_alternative<protocol::TaskAssign>(*event.message)))
            m_impl->callbacks.cancelFile(resultFileId(m_impl->active->chunkId));
        if (event.kind != network::EventKind::JobMessage) {
            m_impl->paused.clear();
            m_impl->accepted.clear();
        }
    }
    if (m_impl->queue.size() >= 256) {
        m_impl->stopping = true;
        m_impl->cancelled = true;
    } else
        m_impl->queue.push_back({event, m_impl->generation});
    m_impl->wake.notify_all();
}
std::vector<ExecutorEvent> JobExecutor::drainEvents() { return m_impl->events.drain(); }
std::optional<protocol::Progress> JobExecutor::progress() const {
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->currentProgress;
}
} // namespace blendnet::jobs
