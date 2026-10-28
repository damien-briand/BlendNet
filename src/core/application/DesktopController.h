#pragma once

#include "blender/GpuInfoService.h"
#include "blender/ReleaseCatalog.h"
#include "jobs/JobExecutor.h"
#include "network/ClientSocket.h"
#include "network/TlsServer.h"
#include "projects/ProjectLibrary.h"

namespace blendnet::application {
bool scheduledAvailable(const AppSettings& settings, int localHour);
protocol::ClientPrefs clientPreferences(const AppSettings& settings, int localHour, bool active);
struct Offer {
    std::size_t source = 0;
    protocol::JobOffer job;
    std::chrono::steady_clock::time_point deadline;
};
struct SourceView {
    std::size_t index = 0;
    network::ClientSnapshot connection;
    std::optional<protocol::Progress> progress;
    std::vector<jobs::ExecutorEvent> history;
};
struct LogEntry {
    std::string time, key, detail;
};
struct DesktopSnapshot {
    AppSettings settings;
    std::uint64_t settingsRevision = 0;
    std::vector<std::string> blenderVersions;
    bool blenderVersionsLoading = false, blenderVersionsFailed = false;
    bool busy = false, listening = false;
    std::string status = "ready", detail;
    std::vector<projects::ProjectEntry> projects;
    std::optional<projects::ProjectInfo> selected;
    std::optional<blender::HardwareProfile> hardware;
    network::ServerSnapshot server;
    std::vector<SourceView> sources;
    std::vector<Offer> offers;
    std::vector<LogEntry> logs;
    std::vector<jobs::ExecutorEvent> clientHistory;
    std::string pairingToken;
};
class DesktopController {
  public:
    DesktopController(AppSettings settings, std::filesystem::path configPath, Logger& logger,
                      blender::FetchListing fetchListings = blender::fetchOfficialListing);
    ~DesktopController();
    DesktopSnapshot snapshot() const;
    void saveSettings(AppSettings settings);
    void refreshBlenderVersions();
    void scan();
    void inspect(std::filesystem::path scene);
    void share(jobs::Job job);
    void startServer();
    void stopServer();
    void regenerateToken();
    void connectSource(std::size_t source);
    void disconnectSource(std::size_t source);
    void decide(std::size_t source, std::string jobId, bool accept);
    void cancelParticipation(std::size_t source);
    void probe();
    void benchmark();
    void cancelOperation();
    void jobAction(std::string jobId, std::string action);
    void clientAction(std::string sessionId, bool kick);
    void excludeClient(std::string sessionId, std::string jobId);
    void exportLogs(std::filesystem::path destination);
    void userActivity();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace blendnet::application
