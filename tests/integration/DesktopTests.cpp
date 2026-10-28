#include "TemporaryDirectory.h"
#include "application/DesktopController.h"
#include "blender/Process.h"
#include "network/Security.h"
#include "transfer/TransferEngine.h"
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <zip.h>
using namespace blendnet;
using namespace std::chrono_literals;
namespace {
template <typename F> bool await(F predicate, std::chrono::milliseconds limit = 15s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
class DesktopIntegration : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    std::ostringstream log;
    Logger logger{log};
    network::TlsServer server{logger};
    network::ServerIdentity identity;
    AppSettings settings;
    std::filesystem::path config, archive, cache;
    std::unique_ptr<application::DesktopController> desktop;
    void SetUp() override {
        const auto platform = blender::Platform::current();
        ASSERT_TRUE(platform);
        if (platform->system != blender::System::Linux)
            GTEST_SKIP() << "Linux tar fixture";
        const auto release = blender::officialRelease("4.2.1", *platform);
        ASSERT_TRUE(release);
        const auto package = directory.path() / "package";
        std::filesystem::create_directories((package / release.value->executable).parent_path());
        std::filesystem::copy_file(BLENDNET_FAKE_BLENDER_PATH, package / release.value->executable);
        archive = directory.path() / release.value->archiveName;
        cache = directory.path() / "cache";
        ASSERT_EQ(blender::detail::runProcess({"tar", "-cJf", blender::utf8(archive), "-C",
                                               blender::utf8(package), release.value->topDirectory},
                                              nullptr, 10s)
                      .error,
                  blender::BlenderError::None);
        const auto checksum = transfer::sha256File(archive);
        ASSERT_TRUE(checksum);
        blender::BlenderInstaller installer(cache, [&](const auto& url, const auto& path,
                                                       std::uint64_t, const std::atomic_bool*) {
            if (url.ends_with(".sha256"))
                test::writeFile(path, *checksum.value + "  " + release.value->archiveName + "\n");
            else
                std::filesystem::copy_file(archive, path);
            return blender::BlenderError::None;
        });
        ASSERT_TRUE(installer.ensureVersion("4.2.1"));
        ASSERT_EQ(network::ensureServerIdentity(directory.path() / "identity", identity),
                  network::NetworkError::None);
        network::ServerOptions options;
        options.port = 0;
        options.token = identity.token;
        options.certificateFile = identity.certificateFile;
        options.privateKeyFile = identity.privateKeyFile;
        options.transfers.directory = directory.path() / "received";
        options.jobTimings.offerTimeout = 5s;
        ASSERT_EQ(server.start(options), network::NetworkError::None);
        settings.blenderCacheDirectory = cache;
        settings.blenderVersion = "4.2.1";
        settings.workDirectory = directory.path() / "work";
        settings.benchmarkPrompted = true;
        settings.offerTimeoutSec = 1;
        settings.knownSources = {{"127.0.0.1", server.snapshot().port, identity.token, {}}};
        config = directory.path() / "settings.json";
        ASSERT_EQ(settings.save(config), SettingsError::None);
        desktop = std::make_unique<application::DesktopController>(
            settings, config, logger,
            [](const std::string&, const std::atomic_bool*) -> blender::BlenderResult<std::string> {
                return {{}, blender::BlenderError::DownloadFailed};
            });
        ASSERT_TRUE(await([&] {
            const auto s = desktop->snapshot();
            return s.status == "scan.done" && !s.busy;
        }));
        desktop->connectSource(0);
        ASSERT_TRUE(await([&] {
            const auto s = desktop->snapshot();
            return s.sources.size() == 1 &&
                   s.sources[0].connection.state == network::ConnectionState::Ready;
        }));
    }
    void TearDown() override {
        desktop.reset();
        server.stop();
        if (!identity.token.empty()) {
            EXPECT_EQ(log.str().find(identity.token), std::string::npos);
        }
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(directory.path(), error), end;
             it != end && !error; it.increment(error))
            if (!it->is_symlink())
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, error);
    }
    jobs::Job job(std::string id) {
        jobs::Job job;
        job.id = std::move(id);
        job.projectName = "Desktop project";
        job.frameEnd = 3;
        job.chunkSize = 2;
        job.projectZip = directory.path() / (job.id + ".zip");
        job.outputDirectory = directory.path() / job.id;
        auto* zip = zip_open(job.projectZip.string().c_str(), ZIP_CREATE | ZIP_EXCL, nullptr);
        if (!zip)
            throw std::runtime_error("Fixture ZIP");
        static const char data[] = "Fixture scene";
        auto* source = zip_source_buffer(zip, data, sizeof(data), 0);
        if (!source || zip_file_add(zip, "scene.blend", source, 0) < 0 || zip_close(zip) < 0)
            throw std::runtime_error("Fixture ZIP");
        return job;
    }
};
} // namespace
TEST_F(DesktopIntegration, ExpiryDeclinesWithoutExecutionAndNextOfferCanBeAccepted) {
    server.publishJob(job("expired"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "expired/frame_0001.png"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.empty(); }, 3s));
    ASSERT_TRUE(desktop->snapshot().clientHistory.empty());
    server.publishJob(job("accepted"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    desktop->decide(0, "accepted", true);
    ASSERT_TRUE(await(
        [&] {
            const auto s = server.snapshot();
            return std::any_of(s.jobs.begin(), s.jobs.end(), [](const auto& j) {
                return j.job.id == "accepted" && j.job.state == jobs::Job::State::Done &&
                       j.validFrames == 3;
            });
        },
        20s));
    ASSERT_TRUE(await([&] {
        return !desktop->snapshot().clientHistory.empty() &&
               desktop->snapshot().clientHistory.back().state == jobs::ExecutorState::Completed;
    }));
    AppSettings saved;
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    ASSERT_TRUE(saved.knownSources[0].certificateSha256);
    auto updated = desktop->snapshot().settings;
    updated.language = "en";
    desktop->saveSettings(updated);
    ASSERT_TRUE(await([&] {
        return desktop->snapshot().settings.language == "en" && !desktop->snapshot().busy;
    }));
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    EXPECT_TRUE(saved.knownSources[0].certificateSha256);
}
TEST_F(DesktopIntegration, ExplicitDeclineLeavesConnectionReadyAndFutureOffersAvailable) {
    server.publishJob(job("declined"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    desktop->decide(0, "declined", false);
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.empty(); }));
    EXPECT_EQ(desktop->snapshot().sources[0].connection.state, network::ConnectionState::Ready);
    server.publishJob(job("next"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    EXPECT_EQ(desktop->snapshot().offers[0].job.jobId, "next");
}
TEST_F(DesktopIntegration, ParticipationCanBeCancelledBeforeTheFirstRenderStarts) {
    server.publishJob(job("cancelled"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    desktop->decide(0, "cancelled", true);
    desktop->cancelParticipation(0);
    ASSERT_TRUE(await([&] {
        const auto s = server.snapshot();
        return !s.jobs.empty() && s.jobs[0].chunks[0].attempts >= 1;
    }));
    EXPECT_EQ(server.snapshot().jobs[0].validFrames, 0u);
    const auto snapshot = desktop->snapshot();
    EXPECT_EQ(snapshot.sources[0].connection.state, network::ConnectionState::Ready);
    EXPECT_TRUE(
        std::none_of(snapshot.clientHistory.begin(), snapshot.clientHistory.end(),
                     [](const auto& e) { return e.state == jobs::ExecutorState::Rendering; }));
    server.publishJob(job("next-after-cancel"));
    ASSERT_TRUE(await([&] { return desktop->snapshot().offers.size() == 1; }));
    EXPECT_EQ(desktop->snapshot().offers[0].job.jobId, "next-after-cancel");
}
TEST_F(DesktopIntegration, BackendChangeClearsThePreviousBenchmarkScore) {
    auto same = desktop->snapshot().settings;
    same.benchmarkScore = 123;
    desktop->saveSettings(same);
    ASSERT_TRUE(await([&] {
        const auto s = desktop->snapshot();
        return !s.busy && s.settings.benchmarkScore == 123;
    }));
    desktop->disconnectSource(0);
    ASSERT_TRUE(await([&] { return desktop->snapshot().sources.empty(); }));
    auto changed = desktop->snapshot().settings;
    changed.computeBackend = "CUDA";
    desktop->saveSettings(changed);
    ASSERT_TRUE(await([&] {
        const auto s = desktop->snapshot();
        return !s.busy && s.settings.computeBackend == "CUDA";
    }));
    EXPECT_FALSE(desktop->snapshot().settings.benchmarkScore);
    AppSettings saved;
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    EXPECT_FALSE(saved.benchmarkScore);
}
