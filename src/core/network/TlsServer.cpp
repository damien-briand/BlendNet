#include "network/TlsServer.h"
#include "jobs/JobOrchestrator.h"
#include "jobs/ResultAssembler.h"
#include "network/Security.h"
#include "network/TlsChannel.h"
#include "network/TransferLink.h"
#include "transfer/TransferEngine.h"

#include <algorithm>
#include <map>
#include <thread>

namespace blendnet::network {
using detail::TlsChannel;
using namespace std::chrono;

struct TlsServer::Impl {
    struct Session : std::enable_shared_from_this<Session> {
        enum class Stage { Hello, Gpu, Preferences, Ready, Closing };
        Impl& owner;
        std::string id;
        std::shared_ptr<TlsChannel> channel;
        asio::steady_timer deadline;
        asio::steady_timer heartbeat;
        Stage stage = Stage::Hello;
        ClientInfo info;
        std::string peerScope;
        std::shared_ptr<detail::TransferLink> transfers;
        steady_clock::time_point lastPong;

        Session(Impl& server, std::string sessionId)
            : owner(server), id(std::move(sessionId)),
              channel(std::make_shared<TlsChannel>(owner.io, owner.tls)), deadline(owner.io),
              heartbeat(owner.io) {
            info.id = id;
        }
        void start() {
            const std::weak_ptr<Session> weak = shared_from_this();
            channel->callbacks(
                [weak](protocol::Message message) {
                    if (const auto self = weak.lock())
                        self->receive(std::move(message));
                },
                [weak](NetworkError error) {
                    if (const auto self = weak.lock()) {
                        self->stage = Stage::Closing;
                        if (self->transfers) {
                            self->transfers->stop();
                            self->transfers.reset();
                        }
                        self->deadline.cancel();
                        self->heartbeat.cancel();
                        self->owner.remove(self->id, error);
                    }
                });
            deadline.expires_after(owner.options.timings.handshakeTimeout);
            deadline.async_wait([self = shared_from_this()](const asio::error_code& error) {
                if (!error)
                    self->channel->close(NetworkError::Timeout);
            });
            channel->stream().async_handshake(
                asio::ssl::stream_base::server,
                [self = shared_from_this()](const asio::error_code& error) {
                    if (error)
                        self->channel->close(NetworkError::HandshakeFailed);
                    else
                        self->channel->read();
                });
        }
        void reject(std::string reason, NetworkError error) {
            stage = Stage::Closing;
            channel->sendAndClose(protocol::HelloErr{std::move(reason)}, error);
        }
        void receive(protocol::Message message) {
            if (stage == Stage::Closing)
                return;
            if (stage == Stage::Hello) {
                const auto* hello = std::get_if<protocol::Hello>(&message);
                if (!hello)
                    return channel->close(NetworkError::ProtocolViolation);
                if (hello->protocolVersion != protocol::protocolVersion)
                    return reject("bad_version", NetworkError::VersionMismatch);
                if (!sameToken(hello->token, owner.options.token))
                    return reject("bad_token", NetworkError::AuthenticationFailed);
                info.userName = hello->userName;
                info.machineName = hello->machineName;
                stage = Stage::Gpu;
                channel->send(
                    protocol::HelloOk{protocol::protocolVersion, owner.options.serverName});
                return;
            }
            if (stage == Stage::Gpu) {
                const auto* gpu = std::get_if<protocol::GpuInfo>(&message);
                if (!gpu)
                    return channel->close(NetworkError::ProtocolViolation);
                info.gpu = *gpu;
                stage = Stage::Preferences;
                return;
            }
            if (stage == Stage::Preferences) {
                const auto* preferences = std::get_if<protocol::ClientPrefs>(&message);
                if (!preferences)
                    return channel->close(NetworkError::ProtocolViolation);
                info.preferences = *preferences;
                stage = Stage::Ready;
                deadline.cancel();
                lastPong = steady_clock::now();
                owner.publish(info);
                peerScope = info.userName + std::string(1, '\0') + info.machineName;
                if (!owner.options.transfers.directory.empty()) {
                    const auto worker =
                        owner.transferWorkers
                            .try_emplace(peerScope, asio::make_strand(owner.workers))
                            .first->second;
                    transfers = std::make_shared<detail::TransferLink>(
                        owner.io, worker, channel, owner.options.transfers, peerScope,
                        [owner = &owner, session = id,
                         scope = peerScope](transfer::TransferProgress progress) {
                            owner->report(session, scope, std::move(progress));
                        });
                    // Les projets P5 sont représentés après une nouvelle acceptation.
                    // Le checkpoint P4 conserve les octets, sans envoyer avant JOB_ACCEPT.
                    std::erase_if(owner.pendingFiles[peerScope], [&](const auto& pending) {
                        for (const auto& job : owner.jobs->snapshot())
                            if (pending.first == job.job.id + "_PROJECT_ZIP")
                                return true;
                        return false;
                    });
                    for (const auto& [fileId, request] : owner.pendingFiles[peerScope])
                        transfers->sendFile(request);
                }
                owner.events.push({EventKind::Ready, NetworkError::None, id});
                owner.jobs->connected(info);
                owner.log(LogLevel::Info, "TLS client registered");
                tick();
                return;
            }
            if (std::holds_alternative<protocol::Ping>(message))
                channel->send(protocol::Pong{});
            else if (std::holds_alternative<protocol::Pong>(message))
                lastPong = steady_clock::now();
            else if (const auto* preferences = std::get_if<protocol::StatusUpdate>(&message)) {
                info.preferences = preferences->preferences;
                owner.publish(info);
                owner.events.push({EventKind::PreferencesChanged, NetworkError::None, id});
                owner.jobs->preferences(info);
            } else if (std::holds_alternative<protocol::FileErr>(message)) {
                owner.jobs->message(id, message);
                if (transfers)
                    transfers->handle(std::move(message));
            } else if (owner.jobs->message(id, message)) {
                // Contrôles métier consommés par l'orchestrateur core.
            } else if (!transfers || !transfers->handle(std::move(message)))
                channel->close(NetworkError::ProtocolViolation);
        }
        void tick() {
            if (stage == Stage::Closing)
                return;
            heartbeat.expires_after(owner.options.timings.pingInterval);
            heartbeat.async_wait([self = shared_from_this()](const asio::error_code& error) {
                if (error)
                    return;
                if (steady_clock::now() - self->lastPong >= self->owner.options.timings.peerTimeout)
                    return self->channel->close(NetworkError::Timeout);
                self->channel->send(protocol::Ping{});
                self->tick();
            });
        }
    };

    Logger& logger;
    ServerOptions options;
    asio::io_context io;
    asio::thread_pool workers{2};
    asio::ssl::context tls{asio::ssl::context::tls_server};
    asio::ip::tcp::acceptor acceptor{io};
    asio::steady_timer acceptRetry{io};
    asio::steady_timer jobTick{io};
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(io)};
    std::jthread thread;
    bool stopping = false;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::map<std::string, asio::strand<asio::thread_pool::executor_type>> transferWorkers;
    std::map<std::string, std::map<std::string, transfer::FileRequest>> pendingFiles;
    mutable std::mutex mutex;
    ServerSnapshot state;
    EventQueue<NetworkEvent> events;
    std::unique_ptr<jobs::JobOrchestrator> jobs;
    std::atomic_bool jobsStopped{false};

    explicit Impl(Logger& log) : logger(log) {}
    ~Impl() { stop(); }
    void log(LogLevel level, std::string_view message) { logger.log(level, message); }
    NetworkError start(const ServerOptions& settings) {
        options = settings;
        if (!options.timings.valid() || options.token.size() < 64 || options.maxClients == 0 ||
            options.jobTimings.offerTimeout.count() <= 0 ||
            options.jobTimings.taskTimeout.count() <= 0 ||
            !protocol::ProtocolCodec::encode(
                protocol::HelloOk{protocol::protocolVersion, options.serverName}))
            return NetworkError::InvalidOptions;
        try {
            if (!detail::configureTls(tls))
                return NetworkError::TlsConfiguration;
            tls.use_certificate_chain_file(options.certificateFile.string());
            tls.use_private_key_file(options.privateKeyFile.string(), asio::ssl::context::pem);
            if (SSL_CTX_check_private_key(tls.native_handle()) != 1)
                return NetworkError::TlsConfiguration;
        } catch (const std::exception&) {
            return NetworkError::TlsConfiguration;
        }
        asio::error_code error;
        const auto address = asio::ip::make_address(options.bindAddress, error);
        if (error)
            return NetworkError::InvalidOptions;
        const asio::ip::tcp::endpoint endpoint(address, options.port);
        acceptor.open(endpoint.protocol(), error);
        if (!error)
            acceptor.set_option(asio::socket_base::reuse_address(true), error);
        if (!error)
            acceptor.bind(endpoint, error);
        if (!error)
            acceptor.listen(asio::socket_base::max_listen_connections, error);
        if (error)
            return NetworkError::ListenFailed;
        state.port = acceptor.local_endpoint(error).port();
        if (error)
            return NetworkError::ListenFailed;
        initializeJobs();
        accept();
        events.push({EventKind::Listening});
        log(LogLevel::Info, "TLS server listening");
        thread = std::jthread([this] { io.run(); });
        return NetworkError::None;
    }
    void initializeJobs() {
        jobs::JobOrchestrator::Callbacks callbacks;
        callbacks.send = [this](const std::string& id, const protocol::Message& message) {
            const auto found = sessions.find(id);
            if (found != sessions.end())
                found->second->channel->send(message);
        };
        callbacks.sendFile = [this](const std::string& id, const transfer::FileRequest& request) {
            offerFile(id, request);
        };
        callbacks.verify = [this](const jobs::Job& job, const protocol::TaskAssign& task,
                                  const std::string& id, const std::filesystem::path& archive) {
            verify(job, task, id, archive);
        };
        callbacks.assemble = [this](const jobs::Job& job,
                                    const std::map<int, jobs::ValidatedFrame>& frames,
                                    bool allowMissing) {
            asio::post(workers, [this, job, frames, allowMissing] {
                const bool valid =
                    jobs::ResultAssembler::assemble(job, frames, &jobsStopped, allowMissing);
                asio::post(io, [this, job, valid] {
                    if (!stopping)
                        jobs->assembled(job.id, valid);
                });
            });
        };
        if (options.localRenderer)
            callbacks.local = [this](const jobs::Job& job, const protocol::TaskAssign& task) {
                const auto renderer = options.localRenderer;
                asio::post(workers, [this, job, task, renderer] {
                    const auto path = job.outputDirectory / ".blendnet" /
                                      transfer::storageKey(task.chunkId + "local") / "result.zip";
                    bool success = false;
                    try {
                        std::filesystem::create_directories(path.parent_path());
                        success = !jobsStopped && renderer(job, task, path);
                    } catch (const std::exception&) {
                    }
                    asio::post(io, [this, job, task, path, success] {
                        if (stopping)
                            return;
                        if (success)
                            verify(job, task, "__blendnet_local_renderer", path);
                        else
                            jobs->validated(job.id, task.chunkId, "__blendnet_local_renderer", {});
                    });
                });
            };
        callbacks.changed = [this] {
            const auto views = jobs->snapshot();
            {
                const std::lock_guard lock(mutex);
                state.jobs = views;
            }
            if (!views.empty())
                events.push({EventKind::JobsChanged});
        };
        jobs = std::make_unique<jobs::JobOrchestrator>(std::move(callbacks), options.jobTimings);
        tickJobs();
    }
    void tickJobs() {
        jobTick.expires_after(milliseconds(100));
        jobTick.async_wait([this](const asio::error_code& error) {
            if (error || stopping)
                return;
            jobs->tick();
            tickJobs();
        });
    }
    void verify(const jobs::Job& job, const protocol::TaskAssign& task, const std::string& id,
                const std::filesystem::path& archive) {
        asio::post(workers, [this, job, task, id, archive] {
            const auto staging =
                job.outputDirectory / ".blendnet" / transfer::storageKey(task.chunkId);
            auto frames = jobs::ResultAssembler::validate(task, archive, staging, &jobsStopped);
            asio::post(io, [this, job, task, id, frames = std::move(frames)]() mutable {
                if (!stopping)
                    jobs->validated(job.id, task.chunkId, id, std::move(frames));
            });
        });
    }
    void publishJob(jobs::Job job) {
        if (stopping)
            return;
        asio::post(workers, [this, job = std::move(job)]() mutable {
            bool readable = false;
            try {
                readable = std::filesystem::is_regular_file(job.projectZip);
                if (readable)
                    job.zipBytes = std::filesystem::file_size(job.projectZip);
            } catch (const std::exception&) {
            }
            asio::post(io, [this, job = std::move(job), readable]() mutable {
                if (stopping)
                    return;
                const auto id = job.id;
                if (!readable || !jobs->publish(std::move(job))) {
                    NetworkEvent event{EventKind::JobRejected};
                    event.jobId = id;
                    events.push(std::move(event));
                }
            });
        });
    }
    void accept() {
        const auto id = randomSessionId();
        if (!id)
            return log(LogLevel::Error, "Cannot allocate secure session identifier");
        const auto session = std::make_shared<Session>(*this, *id);
        acceptor.async_accept(session->channel->stream().next_layer(),
                              [this, session](const asio::error_code& error) {
                                  if (stopping)
                                      return;
                                  if (!error && sessions.size() < options.maxClients) {
                                      sessions.emplace(session->id, session);
                                      session->start();
                                  } else {
                                      session->channel->close(error ? NetworkError::IoError
                                                                    : NetworkError::QueueFull);
                                  }
                                  if (error) {
                                      acceptRetry.expires_after(milliseconds(100));
                                      acceptRetry.async_wait([this](const asio::error_code& e) {
                                          if (!e && !stopping)
                                              accept();
                                      });
                                  } else
                                      accept();
                              });
    }
    void report(const std::string& id, const std::string& scope,
                transfer::TransferProgress progress) {
        progress.peerId = id;
        {
            const std::lock_guard lock(mutex);
            recordTransfer(state.transfers, progress);
        }
        NetworkEvent event{EventKind::TransferProgress, NetworkError::None, id};
        event.transfer = progress;
        events.push(std::move(event));
        if (progress.direction == transfer::Direction::Sending &&
            (progress.state == transfer::TransferState::Completed ||
             progress.state == transfer::TransferState::Failed))
            pendingFiles[scope].erase(progress.fileId);
        if (progress.state == transfer::TransferState::Completed)
            log(LogLevel::Info, "File transfer completed");
        if (progress.state == transfer::TransferState::Failed)
            log(LogLevel::Warning, transfer::toString(progress.error));
        if (jobs && !stopping)
            jobs->transferred(id, progress);
    }
    void offerFile(const std::string& id, const transfer::FileRequest& request) {
        const auto found = sessions.find(id);
        if (found == sessions.end() || !found->second->transfers)
            return;
        auto& pending = pendingFiles[found->second->peerScope];
        if (pending.contains(request.fileId))
            return;
        if (pending.size() >= 16) {
            report(id, found->second->peerScope,
                   {request.fileId, transfer::Direction::Sending, transfer::TransferState::Failed,
                    transfer::TransferError::Busy});
            return;
        }
        pending[request.fileId] = request;
        found->second->transfers->sendFile(request);
    }
    void publish(const ClientInfo& client) {
        const std::lock_guard lock(mutex);
        const auto found =
            std::find_if(state.clients.begin(), state.clients.end(),
                         [&client](const ClientInfo& item) { return item.id == client.id; });
        if (found == state.clients.end())
            state.clients.push_back(client);
        else
            *found = client;
    }
    void remove(const std::string& id, NetworkError error) {
        {
            const std::lock_guard lock(mutex);
            std::erase_if(state.clients, [&id](const ClientInfo& item) { return item.id == id; });
            for (auto& item : state.transfers)
                if (item.peerId == id && (item.state == transfer::TransferState::Transferring ||
                                          item.state == transfer::TransferState::Preparing))
                    item.state = transfer::TransferState::Suspended;
        }
        events.push({EventKind::Disconnected, error, id});
        log(error == NetworkError::Stopped ? LogLevel::Info : LogLevel::Warning, toString(error));
        sessions.erase(id);
        if (jobs && !stopping)
            jobs->disconnected(id);
    }
    void stop() {
        if (!thread.joinable())
            return;
        asio::post(io, [this] {
            stopping = true;
            jobsStopped = true;
            jobTick.cancel();
            asio::error_code ignored;
            acceptor.cancel(ignored);
            acceptor.close(ignored);
            acceptRetry.cancel();
            auto pending = sessions;
            for (const auto& [id, session] : pending)
                session->channel->close(NetworkError::Stopped);
            work.reset();
        });
        thread.join();
        workers.join();
        io.restart();
        io.poll();
    }
};

TlsServer::TlsServer(Logger& logger) : m_logger(logger) {}
TlsServer::~TlsServer() = default;
NetworkError TlsServer::start(const ServerOptions& options) {
    stop();
    m_impl = std::make_unique<Impl>(m_logger);
    return m_impl->start(options);
}
void TlsServer::stop() {
    if (m_impl)
        m_impl->stop();
}
void TlsServer::disconnect(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            const auto found = impl->sessions.find(id);
            if (found != impl->sessions.end())
                found->second->channel->sendAndClose(
                    protocol::ClientKick{"source requested disconnect"}, NetworkError::Kicked);
        });
}
void TlsServer::sendFile(const std::string& id, const transfer::FileRequest& request) {
    if (m_impl)
        asio::post(m_impl->io,
                   [impl = m_impl.get(), id, request] { impl->offerFile(id, request); });
}
void TlsServer::interrupt(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            const auto found = impl->sessions.find(id);
            if (found != impl->sessions.end())
                found->second->channel->close(NetworkError::IoError);
        });
}
void TlsServer::publishJob(const jobs::Job& job) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), job] { impl->publishJob(job); });
}
void TlsServer::pauseJob(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            if (impl->jobs && !impl->stopping)
                impl->jobs->pause(id);
        });
}
void TlsServer::resumeJob(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            if (impl->jobs && !impl->stopping)
                impl->jobs->resume(id);
        });
}
void TlsServer::cancelJob(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            if (impl->jobs && !impl->stopping)
                impl->jobs->cancel(id);
        });
}
void TlsServer::cancelTask(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            if (impl->jobs && !impl->stopping)
                impl->jobs->cancelTask(id);
        });
}
void TlsServer::finalizeJobWithFailures(const std::string& id) {
    if (m_impl)
        asio::post(m_impl->io, [impl = m_impl.get(), id] {
            if (impl->jobs && !impl->stopping)
                impl->jobs->finalizeWithFailures(id);
        });
}
ServerSnapshot TlsServer::snapshot() const {
    if (!m_impl)
        return {};
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->state;
}
std::vector<NetworkEvent> TlsServer::drainEvents() {
    return m_impl ? m_impl->events.drain() : std::vector<NetworkEvent>{};
}
} // namespace blendnet::network
