#include "application/DesktopController.h"
#include "blender/Benchmark.h"
#include "config/Utf8Path.h"
#include "log/LogExporter.h"
#include "network/Security.h"
#include "transfer/TransferEngine.h"

#include <algorithm>
#include <asio.hpp>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

namespace blendnet::application {
bool scheduledAvailable(const AppSettings& s, int hour) {
    if (!s.scheduleEnabled || s.availableFromHour == s.availableUntilHour)
        return true;
    return s.availableFromHour < s.availableUntilHour
               ? hour >= s.availableFromHour && hour < s.availableUntilHour
               : hour >= s.availableFromHour || hour < s.availableUntilHour;
}
protocol::ClientPrefs clientPreferences(const AppSettings& s, int hour, bool active) {
    protocol::ClientPrefs prefs{s.excludedEngines, s.availability, {}, 1, {}};
    if (!scheduledAvailable(s, hour) || (s.stopOnActivity && active))
        prefs.availability = "DND";
    if (s.maxFramesPerJob)
        prefs.maxFramesPerJob = s.maxFramesPerJob;
    if (s.quotaGB)
        prefs.quotaGB = s.quotaGB;
    return prefs;
}
namespace {
std::tm localTime() {
    const auto now = std::time(nullptr);
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &now);
#else
    localtime_r(&now, &result);
#endif
    return result;
}
std::string timestamp() {
    auto t = localTime();
    std::ostringstream s;
    s << std::put_time(&t, "%H:%M:%S");
    return s.str();
}
} // namespace
struct DesktopController::Impl {
    Logger& logger;
    std::filesystem::path config;
    DesktopSnapshot state, published;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands, work;
    std::mutex workMutex;
    std::condition_variable workWake;
    std::atomic_bool cancelled{false}, stopping{false};
    std::jthread actor, worker, catalogueWorker;
    blender::FetchListing fetchListings;
    network::TlsServer server;
    network::ServerIdentity identity;
    struct Node {
        std::unique_ptr<network::ClientSocket> socket;
        std::unique_ptr<jobs::JobExecutor> executor;
        std::optional<protocol::Progress> progress;
        std::vector<jobs::ExecutorEvent> history;
        protocol::ClientPrefs preferences;
        std::set<std::string> participating;
    };
    std::map<std::size_t, Node> nodes;
    std::map<std::string, std::chrono::steady_clock::time_point> declined;
    std::chrono::steady_clock::time_point lastActivity{};
    bool rescan = false, autoVersionFailed = false;
    Impl(AppSettings settings, std::filesystem::path path, Logger& log,
         blender::FetchListing fetch)
        : logger(log), config(std::filesystem::absolute(path)), fetchListings(std::move(fetch)),
          server(log) {
        state.settings = std::move(settings);
        published = state;
        worker = std::jthread([this] { workLoop(); });
        actor = std::jthread([this] { loop(); });
    }
    ~Impl() {
        stopping = true;
        cancelled = true;
        wake.notify_all();
        workWake.notify_all();
        worker.join();
        actor.join();
        if (catalogueWorker.joinable())
            catalogueWorker.join();
    }
    void post(std::function<void()> command) {
        const std::lock_guard lock(mutex);
        if (commands.size() < 256)
            commands.push_back(std::move(command));
        wake.notify_all();
    }
    std::filesystem::path resolve(const std::filesystem::path& path) const {
        return path.is_absolute() ? path : config.parent_path() / path;
    }
    void note(std::string key, std::string detail = {}) {
        state.status = key;
        state.detail = detail;
        if (state.logs.size() == 500)
            state.logs.erase(state.logs.begin());
        state.logs.push_back({timestamp(), std::move(key), std::move(detail)});
        logger.log(LogLevel::Info, "Desktop: " + state.status);
    }
    void task(std::string key, std::function<void()> action) {
        if (state.busy) {
            note("busy");
            return;
        }
        cancelled = false;
        state.busy = true;
        note(std::move(key));
        const std::lock_guard lock(workMutex);
        work.push_back([this, action = std::move(action)] {
            try {
                action();
            } catch (const std::exception&) {
                post([this] { note("operation.failed"); });
            }
            post([this] { state.busy = false; });
        });
        workWake.notify_one();
    }
    void workLoop() {
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock lock(workMutex);
                workWake.wait(lock, [this] { return stopping || !work.empty(); });
                if (stopping)
                    return;
                action = std::move(work.front());
                work.pop_front();
            }
            action();
        }
    }
    void close(std::size_t index) {
        auto found = nodes.find(index);
        if (found == nodes.end())
            return;
        found->second.socket->stop();
        found->second.executor.reset();
        nodes.erase(found);
        std::erase_if(state.offers, [&](const auto& offer) { return offer.source == index; });
    }
    void scan() {
        const auto settings = state.settings;
        auto directories = settings.projectDirectories, hidden = settings.hiddenProjects;
        for (auto& path : directories)
            path = resolve(path);
        for (auto& path : hidden)
            path = resolve(path);
        task("scanning", [this, directories, hidden] {
            auto projects = projects::ProjectLibrary::scan(directories, hidden, &cancelled);
            post([this, projects = std::move(projects)]() mutable {
                state.projects = std::move(projects);
                if (state.selected &&
                    std::none_of(state.projects.begin(), state.projects.end(), [&](const auto& p) {
                        return p.path == state.selected->entry.path;
                    }))
                    state.selected.reset();
                note("scan.done");
            });
        });
    }
    void refreshVersions() {
        if (stopping || state.blenderVersionsLoading)
            return;
        if (catalogueWorker.joinable())
            catalogueWorker.join();
        state.blenderVersionsLoading = true;
        state.blenderVersionsFailed = false;
        autoVersionFailed = false;
        catalogueWorker = std::jthread([this] {
            const auto platform = blender::Platform::current();
            auto result = platform ? blender::availableReleases(*platform, &stopping, fetchListings)
                                   : blender::BlenderResult<std::vector<std::string>>{
                                         {}, blender::BlenderError::UnsupportedPlatform};
            post([this, result = std::move(result)]() mutable {
                state.blenderVersionsLoading = false;
                state.blenderVersionsFailed = !result;
                if (result)
                    state.blenderVersions = std::move(*result.value);
            });
        });
    }
    void selectDefaultVersion() {
        if (state.busy || autoVersionFailed || !state.settings.blenderVersion.empty() ||
            state.blenderVersions.empty() || state.blenderVersionsLoading ||
            state.blenderVersionsFailed)
            return;
        const auto version = state.blenderVersions.front();
        task("saving", [this, version] {
            std::string selected;
            const auto error = AppSettings::update(config, [&](AppSettings& current) {
                if (current.blenderVersion.empty()) {
                    current.blenderVersion = version;
                    current.benchmarkScore.reset();
                }
                selected = current.blenderVersion;
                return SettingsError::None;
            });
            post([this, error, selected = std::move(selected)] {
                if (error == SettingsError::None) {
                    state.settings.blenderVersion = selected;
                    state.settings.benchmarkScore.reset();
                    ++state.settingsRevision;
                    note("settings.saved");
                } else {
                    autoVersionFailed = true;
                    note("settings.invalid");
                }
            });
        });
    }
    void connect(std::size_t index) {
        if (index >= state.settings.knownSources.size())
            return;
        if (!state.hardware || state.hardware->gpu.backend != state.settings.computeBackend ||
            state.hardware->gpu.blenderVersion != state.settings.blenderVersion) {
            probe(index);
            return;
        }
        close(index);
        network::ClientOptions options;
        options.source = state.settings.knownSources[index];
        options.settingsPath = config;
        options.userName = "BlendNet";
        options.machineName = asio::ip::host_name();
        options.gpu = state.hardware->gpu;
        options.gpu.benchmarkScore = state.settings.benchmarkScore;
        options.preferences = clientPreferences(state.settings, localTime().tm_hour, false);
        const auto root =
            resolve(state.settings.workDirectory) / "client" /
            transfer::storageKey(options.source.host + ":" + std::to_string(options.source.port));
        options.transfers.directory = root / "transfers";
        options.transfers.quotaDirectory = root;
        if (state.settings.quotaGB)
            options.transfers.quotaBytes =
                std::uint64_t(state.settings.quotaGB) * 1024 * 1024 * 1024;
        auto& node = nodes[index];
        node.socket = std::make_unique<network::ClientSocket>(logger);
        const auto error = node.socket->start(options);
        if (error != network::NetworkError::None) {
            nodes.erase(index);
            note("connection.failed", std::string(network::toString(error)));
            return;
        }
        jobs::ExecutorOptions execution;
        execution.directory = root / "executor";
        execution.quotaDirectory = root;
        execution.quotaBytes = options.transfers.quotaBytes;
        execution.blenderCache = resolve(state.settings.blenderCacheDirectory);
        execution.backend = state.settings.computeBackend;
        execution.threads = state.settings.backgroundRendering
                                ? state.settings.renderThreads
                                : std::max(1u, std::thread::hardware_concurrency());
        execution.renderTimeout = std::chrono::seconds(state.settings.renderTimeoutSec);
        execution.keepCompletedJobs = state.settings.keepCompletedJobs;
        auto* socket = node.socket.get();
        node.executor = std::make_unique<jobs::JobExecutor>(
            execution, jobs::JobExecutor::Callbacks{
                           [socket](const auto& message) { socket->sendJobMessage(message); },
                           [socket](const auto& file) { socket->sendFile(file); },
                           [socket](const auto& id) { socket->cancelFile(id); }});
        node.preferences = options.preferences;
        note("connecting");
    }
    void decision(std::size_t index, const std::string& id, bool accept) {
        const auto offer =
            std::find_if(state.offers.begin(), state.offers.end(),
                         [&](const auto& o) { return o.source == index && o.job.jobId == id; });
        const auto node = nodes.find(index);
        if (offer == state.offers.end() || node == nodes.end())
            return;
        if (accept && std::chrono::steady_clock::now() < offer->deadline &&
            node->second.preferences.availability == "AVAILABLE") {
            node->second.participating.insert(id);
            network::NetworkEvent event{network::EventKind::JobMessage};
            event.message = offer->job;
            node->second.executor->handle(event);
            note("offer.accepted");
        } else {
            node->second.socket->sendJobMessage(protocol::JobDecline{id});
            note("offer.declined");
        }
        state.offers.erase(offer);
    }
    void pump() {
        state.server = server.snapshot();
        for (const auto& event : server.drainEvents()) {
            if (event.kind == network::EventKind::Ready)
                note("client.connected");
            else if (event.kind == network::EventKind::Disconnected)
                note("client.disconnected");
            else if (event.kind == network::EventKind::JobRejected)
                note("job.rejected");
            else if (event.kind == network::EventKind::Error)
                note("server.failed", std::string(network::toString(event.error)));
        }
        const auto now = std::chrono::steady_clock::now();
        const auto prefs = clientPreferences(state.settings, localTime().tm_hour,
                                             now - lastActivity < std::chrono::seconds(10));
        state.sources.clear();
        for (auto& [index, node] : nodes) {
            if (node.preferences != prefs) {
                node.preferences = prefs;
                node.socket->updatePreferences(prefs);
                if (prefs.availability != "AVAILABLE")
                    node.socket->sendJobMessage(
                        protocol::JobDecline{node.progress ? node.progress->jobId : "unavailable"});
            }
            for (const auto& event : node.socket->drainEvents()) {
                if (event.message && std::holds_alternative<protocol::JobOffer>(*event.message)) {
                    const auto& job = std::get<protocol::JobOffer>(*event.message);
                    if (prefs.availability != "AVAILABLE") {
                        node.socket->sendJobMessage(protocol::JobDecline{job.jobId});
                        continue;
                    }
                    if (std::none_of(
                            state.offers.begin(), state.offers.end(), [&](const auto& offer) {
                                return offer.source == index && offer.job.jobId == job.jobId;
                            }))
                        state.offers.push_back(
                            {index, job,
                             now + std::chrono::seconds(state.settings.offerTimeoutSec)});
                } else {
                    node.executor->handle(event);
                    if (event.message)
                        if (const auto* progress = std::get_if<protocol::Progress>(&*event.message))
                            node.progress = *progress;
                    if (event.kind == network::EventKind::Ready) {
                        note("source.connected");
                        if (index < state.settings.knownSources.size())
                            state.settings.knownSources[index].certificateSha256 =
                                node.socket->snapshot().certificateSha256;
                    }
                    if (event.kind == network::EventKind::Disconnected ||
                        event.kind == network::EventKind::Reconnecting ||
                        event.kind == network::EventKind::Error)
                        std::erase_if(state.offers,
                                      [&](const auto& offer) { return offer.source == index; });
                }
            }
            for (auto& event : node.executor->drainEvents()) {
                if (node.history.size() == 100)
                    node.history.erase(node.history.begin());
                node.history.push_back(event);
                note(std::string(jobs::toString(event.state)), event.detail);
                if (state.clientHistory.size() == 200)
                    state.clientHistory.erase(state.clientHistory.begin());
                state.clientHistory.push_back(event);
            }
            node.progress = node.executor->progress();
            const auto snapshot = node.socket->snapshot();
            state.sources.push_back({index, snapshot, node.progress, node.history});
        }
        for (std::size_t i = 0; i < state.offers.size();) {
            if (now >= state.offers[i].deadline) {
                const auto offer = state.offers[i];
                decision(offer.source, offer.job.jobId, false);
            } else
                ++i;
        }
    }
    void loop() {
        scan();
        refreshVersions();
        while (!stopping) {
            std::deque<std::function<void()>> pending;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(25),
                              [this] { return stopping || !commands.empty(); });
                pending.swap(commands);
            }
            for (auto& action : pending)
                try {
                    action();
                } catch (const std::exception&) {
                    note("operation.failed");
                }
            if (rescan && !state.busy) {
                rescan = false;
                scan();
            }
            selectDefaultVersion();
            pump();
            {
                const std::lock_guard lock(mutex);
                published = state;
            }
        }
        while (!nodes.empty())
            close(nodes.begin()->first);
        server.stop();
    }
    void probe(std::optional<std::size_t> connectAfter = {}) {
        if (state.settings.blenderVersion.empty()) {
            note("versions.required");
            return;
        }
        const auto settings = state.settings;
        const auto cache = resolve(settings.blenderCacheDirectory);
        task("probing", [this, settings, cache, connectAfter] {
            const auto binary =
                blender::BlenderInstaller(cache).ensureVersion(settings.blenderVersion, &cancelled);
            if (!binary) {
                post([this, error = binary.error] {
                    note("operation.failed", std::string(blender::toString(error)));
                });
                return;
            }
            auto profile =
                blender::GpuInfoService::probe(*binary.value, settings.computeBackend, &cancelled);
            post([this, profile = std::move(profile), connectAfter]() mutable {
                if (profile) {
                    state.hardware = std::move(*profile.value);
                    note("probe.done");
                    if (connectAfter)
                        connect(*connectAfter);
                } else
                    note("operation.failed", std::string(blender::toString(profile.error)));
            });
        });
    }
};
DesktopController::DesktopController(AppSettings settings, std::filesystem::path config,
                                     Logger& logger, blender::FetchListing fetchListings)
    : m_impl(std::make_unique<Impl>(std::move(settings), std::move(config), logger,
                                    std::move(fetchListings))) {}
DesktopController::~DesktopController() = default;
DesktopSnapshot DesktopController::snapshot() const {
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->published;
}
void DesktopController::cancelOperation() { m_impl->cancelled = true; }
void DesktopController::scan() {
    m_impl->post([p = m_impl.get()] { p->scan(); });
}
void DesktopController::refreshBlenderVersions() {
    m_impl->post([p = m_impl.get()] { p->refreshVersions(); });
}
void DesktopController::saveSettings(AppSettings settings) {
    m_impl->post([p = m_impl.get(), settings = std::move(settings)]() mutable {
        if (settings.validate() != SettingsError::None) {
            p->note("settings.invalid");
            return;
        }
        if (p->state.busy) {
            p->note("busy");
            return;
        }
        if (settings.blenderVersion.empty() && !p->state.blenderVersions.empty())
            settings.blenderVersion = p->state.blenderVersions.front();
        if (settings.computeBackend != p->state.settings.computeBackend ||
            settings.blenderVersion != p->state.settings.blenderVersion)
            settings.benchmarkScore.reset();
        if (!p->nodes.empty() &&
            (settings.computeBackend != p->state.settings.computeBackend ||
             settings.blenderVersion != p->state.settings.blenderVersion ||
             settings.workDirectory != p->state.settings.workDirectory ||
             settings.blenderCacheDirectory != p->state.settings.blenderCacheDirectory ||
             settings.renderThreads != p->state.settings.renderThreads ||
             settings.quotaGB != p->state.settings.quotaGB ||
             settings.keepCompletedJobs != p->state.settings.keepCompletedJobs ||
             settings.backgroundRendering != p->state.settings.backgroundRendering ||
             settings.renderTimeoutSec != p->state.settings.renderTimeoutSec)) {
            p->note("disconnect.required");
            return;
        }
        for (const auto& [index, node] : p->nodes)
            if (index >= settings.knownSources.size() ||
                settings.knownSources[index].host != p->state.settings.knownSources[index].host ||
                settings.knownSources[index].port != p->state.settings.knownSources[index].port ||
                settings.knownSources[index].token != p->state.settings.knownSources[index].token) {
                p->note("disconnect.required");
                return;
            }
        // Carry pins learned by the live connection; an unchanged endpoint must never lose its pin.
        for (auto& source : settings.knownSources)
            for (const auto& old : p->state.settings.knownSources)
                if (source.host == old.host && source.port == old.port && source.token == old.token)
                    source.certificateSha256 = old.certificateSha256;
        p->task("saving", [p, settings = std::move(settings)]() mutable {
            const auto error = AppSettings::update(p->config, [&](AppSettings& current) {
                for (auto& source : settings.knownSources)
                    for (const auto& old : current.knownSources)
                        if (source.host == old.host && source.port == old.port &&
                            source.token == old.token && old.certificateSha256)
                            source.certificateSha256 = old.certificateSha256;
                current = settings;
                return SettingsError::None;
            });
            p->post([p, error, settings = std::move(settings)]() mutable {
                if (error == SettingsError::None) {
                    p->rescan =
                        p->state.settings.projectDirectories != settings.projectDirectories ||
                        p->state.settings.hiddenProjects != settings.hiddenProjects;
                    p->state.settings = std::move(settings);
                    p->autoVersionFailed = false;
                    ++p->state.settingsRevision;
                    p->note("settings.saved");
                } else
                    p->note("settings.invalid");
            });
        });
    });
}
void DesktopController::inspect(std::filesystem::path scene) {
    m_impl->post([p = m_impl.get(), scene = std::move(scene)] {
        if (p->state.settings.blenderVersion.empty()) {
            p->note("versions.required");
            return;
        }
        const auto settings = p->state.settings;
        const auto cache = p->resolve(settings.blenderCacheDirectory);
        p->task("inspecting", [p, scene, settings, cache] {
            const auto binary = blender::BlenderInstaller(cache).ensureVersion(
                settings.blenderVersion, &p->cancelled);
            if (!binary) {
                p->post([p, error = binary.error] {
                    p->note("operation.failed", std::string(blender::toString(error)));
                });
                return;
            }
            auto info = projects::ProjectLibrary::inspect(*binary.value, scene, &p->cancelled);
            p->post([p, info = std::move(info)]() mutable {
                if (info) {
                    p->state.selected = std::move(*info.value);
                    p->note("project.ready");
                } else
                    p->note("operation.failed", std::string(blender::toString(info.error)));
            });
        });
    });
}
void DesktopController::share(jobs::Job job) {
    m_impl->post([p = m_impl.get(), job = std::move(job)]() mutable {
        if (!p->state.listening || !p->state.selected) {
            p->note("server.required");
            return;
        }
        auto project = *p->state.selected;
        for (const auto& path : job.dependencies) {
            auto dep = pathFromUtf8(path);
            if (dep.is_relative())
                dep = project.entry.path.parent_path() / dep;
            project.dependencies.push_back(std::move(dep));
        }
        const auto settings = p->state.settings;
        const auto id = network::randomSessionId();
        if (!id)
            return;
        job.id = *id;
        job.projectName = project.entry.name;
        job.blenderVersion = project.version;
        job.projectZip = p->resolve(settings.workDirectory) / "source" / job.id / "project.zip";
        if (job.outputDirectory.empty()) {
            p->note("output.required");
            return;
        }
        job.outputDirectory = p->resolve(job.outputDirectory);
        const auto cache = p->resolve(settings.blenderCacheDirectory);
        p->task("packaging", [p, cache, project, job] {
            const auto binary =
                blender::BlenderInstaller(cache).ensureVersion(job.blenderVersion, &p->cancelled);
            if (!binary) {
                p->post([p] { p->note("operation.failed"); });
                return;
            }
            const auto error = projects::ProjectLibrary::package(*binary.value, project,
                                                                 job.projectZip, &p->cancelled);
            p->post([p, error, job] {
                if (error == blender::BlenderError::None) {
                    p->server.publishJob(job);
                    p->note("job.published");
                } else
                    p->note("project.packageFailed", std::string(blender::toString(error)));
            });
        });
    });
}
void DesktopController::startServer() {
    m_impl->post([p = m_impl.get()] {
        const auto settings = p->state.settings;
        p->task("server.starting", [p, settings] {
            network::ServerIdentity identity;
            const auto error =
                network::ensureServerIdentity(p->config.parent_path() / "identity", identity);
            p->post([p, settings, identity, error] {
                if (error != network::NetworkError::None) {
                    p->note("server.failed");
                    return;
                }
                network::ServerOptions options;
                options.bindAddress = "0.0.0.0";
                options.port = std::uint16_t(settings.serverPort);
                options.certificateFile = identity.certificateFile;
                options.privateKeyFile = identity.privateKeyFile;
                options.token = identity.token;
                options.transfers.directory =
                    p->resolve(settings.workDirectory) / "source-received";
                options.jobTimings.offerTimeout = std::chrono::seconds(settings.offerTimeoutSec);
                const auto started = p->server.start(options);
                p->identity = identity;
                p->state.listening = started == network::NetworkError::None;
                if (p->state.listening) {
                    p->state.pairingToken = identity.token;
                    p->note("server.started");
                } else
                    p->note("server.failed", std::string(network::toString(started)));
            });
        });
    });
}
void DesktopController::stopServer() {
    m_impl->post([p = m_impl.get()] {
        p->server.stop();
        p->state.listening = false;
        p->state.pairingToken.clear();
        p->note("server.stopped");
    });
}
void DesktopController::regenerateToken() {
    m_impl->post([p = m_impl.get()] {
        if (p->state.listening) {
            p->note("server.stopRequired");
            return;
        }
        p->task("saving", [p] {
            const auto token = network::randomToken();
            if (!token)
                return;
            const auto path = p->config.parent_path() / "identity" / "pairing-token.txt";
            if (!archive::ZipService::privateDirectory(path.parent_path()) ||
                std::filesystem::is_symlink(path))
                return;
            std::ofstream out(path, std::ios::trunc);
            std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write);
            out << *token;
            out.close();
            p->post([p, token] {
                p->state.pairingToken = *token;
                p->note("token.updated");
            });
        });
    });
}
void DesktopController::connectSource(std::size_t index) {
    m_impl->post([p = m_impl.get(), index] { p->connect(index); });
}
void DesktopController::disconnectSource(std::size_t index) {
    m_impl->post([p = m_impl.get(), index] {
        p->close(index);
        p->note("source.disconnected");
    });
}
void DesktopController::decide(std::size_t index, std::string id, bool accept) {
    m_impl->post(
        [p = m_impl.get(), index, id = std::move(id), accept] { p->decision(index, id, accept); });
}
void DesktopController::cancelParticipation(std::size_t index) {
    m_impl->post([p = m_impl.get(), index] {
        const auto found = p->nodes.find(index);
        if (found == p->nodes.end())
            return;
        for (const auto& job : found->second.participating)
            found->second.socket->sendJobMessage(protocol::JobDecline{job});
        found->second.participating.clear();
        if (const auto progress = found->second.executor->progress()) {
            found->second.socket->sendJobMessage(protocol::JobDecline{progress->jobId});
            network::NetworkEvent event{network::EventKind::JobMessage};
            event.message = protocol::TaskCancel{progress->jobId, progress->chunkId};
            found->second.executor->handle(event);
        }
        p->note("cancelled");
    });
}
void DesktopController::probe() {
    m_impl->post([p = m_impl.get()] { p->probe(); });
}
void DesktopController::benchmark() {
    m_impl->post([p = m_impl.get()] {
        if (p->state.settings.blenderVersion.empty()) {
            p->note("versions.required");
            return;
        }
        const auto settings = p->state.settings;
        const auto cache = p->resolve(settings.blenderCacheDirectory);
        const auto root = p->resolve(settings.workDirectory) / "benchmarks";
        p->task("benchmarking", [p, settings, cache, root] {
            const auto binary = blender::BlenderInstaller(cache).ensureVersion(
                settings.blenderVersion, &p->cancelled);
            AppSettings::update(p->config, [](AppSettings& current) {
                current.benchmarkPrompted = true;
                return SettingsError::None;
            });
            p->post([p] {
                p->state.settings.benchmarkPrompted = true;
                ++p->state.settingsRevision;
            });
            if (!binary) {
                p->post([p] { p->note("operation.failed"); });
                return;
            }
            const auto result = blender::Benchmark::run(*binary.value, settings.computeBackend,
                                                        root, p->config, &p->cancelled);
            p->post([p, result] {
                if (result) {
                    p->state.settings.benchmarkScore = result.value->score;
                    ++p->state.settingsRevision;
                    p->note("benchmark.done");
                } else
                    p->note("operation.failed", std::string(blender::toString(result.error)));
            });
        });
    });
}
void DesktopController::jobAction(std::string id, std::string action) {
    m_impl->post([p = m_impl.get(), id = std::move(id), action = std::move(action)] {
        if (action == "pause")
            p->server.pauseJob(id);
        else if (action == "resume")
            p->server.resumeJob(id);
        else if (action == "cancel")
            p->server.cancelJob(id);
        else if (action == "finalize")
            p->server.finalizeJobWithFailures(id);
    });
}
void DesktopController::clientAction(std::string id, bool kick) {
    m_impl->post([p = m_impl.get(), id = std::move(id), kick] {
        if (kick)
            p->server.disconnect(id);
        else
            p->server.cancelTask(id);
    });
}
void DesktopController::excludeClient(std::string session, std::string job) {
    m_impl->post([p = m_impl.get(), session = std::move(session), job = std::move(job)] {
        p->server.excludeClient(session, job);
    });
}
void DesktopController::exportLogs(std::filesystem::path path) {
    m_impl->post([p = m_impl.get(), path = std::move(path)] {
        std::vector<std::string> entries, secrets{p->state.pairingToken};
        for (const auto& log : p->state.logs)
            entries.push_back(log.time + ' ' + log.key + ' ' + log.detail);
        for (const auto& source : p->state.settings.knownSources)
            secrets.push_back(source.token);
        p->task("exporting", [p, path, entries = std::move(entries), secrets = std::move(secrets)] {
            const auto result = exportLog(path, entries, secrets);
            p->post([p, result] {
                p->note(result == LogError::None ? "export.done" : "operation.failed");
            });
        });
    });
}
void DesktopController::userActivity() {
    m_impl->post([p = m_impl.get()] { p->lastActivity = std::chrono::steady_clock::now(); });
}
} // namespace blendnet::application
