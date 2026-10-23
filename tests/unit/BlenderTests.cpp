#include "TemporaryDirectory.h"
#include "blender/Benchmark.h"
#include "blender/BlenderInstaller.h"
#include "blender/GpuInfoService.h"
#include "blender/Process.h"
#include "jobs/JobExecutor.h"
#include "jobs/ResultAssembler.h"
#include "protocol/ProtocolCodec.h"
#include "transfer/TransferEngine.h"

#include <gtest/gtest.h>
#include <zip.h>

#include <condition_variable>
#include <sstream>

using namespace blendnet;
using namespace blendnet::blender;
using namespace std::chrono_literals;
namespace {
class BlenderTest : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    Release release;
    std::filesystem::path archive, cache;
    bool badChecksum = false, failDownload = false;
    int requests = 0;
    Download download;
    void SetUp() override {
        const auto platform = Platform::current();
        ASSERT_TRUE(platform);
        if (platform->system == System::MacOS)
            GTEST_SKIP() << "DMG creation requires the macOS packaging fixture";
        const auto descriptor = officialRelease("4.2.1", *platform);
        ASSERT_TRUE(descriptor);
        release = *descriptor.value;
        const auto package = directory.path() / "package";
        std::filesystem::create_directories((package / release.executable).parent_path());
        std::filesystem::copy_file(BLENDNET_FAKE_BLENDER_PATH, package / release.executable);
        archive = directory.path() / release.archiveName;
        cache = directory.path() / "cache with spaces";
        if (platform->system == System::Windows) {
            zip_t* zip = zip_open(archive.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
            ASSERT_NE(zip, nullptr);
            auto* source = zip_source_file(zip, (package / release.executable).string().c_str(), 0,
                                           ZIP_LENGTH_TO_END);
            ASSERT_NE(source, nullptr);
            ASSERT_GE(zip_file_add(zip, release.executable.generic_string().c_str(), source, 0), 0);
            ASSERT_EQ(zip_close(zip), 0);
        } else {
            ASSERT_EQ(detail::runProcess(
                          {"tar", "-cJf", utf8(archive), "-C", utf8(package), release.topDirectory},
                          nullptr, 10s)
                          .error,
                      BlenderError::None);
        }
        download = [this](const std::string& url, const std::filesystem::path& path, std::uint64_t,
                          const std::atomic_bool*) {
            ++requests;
            EXPECT_TRUE(officialDownloadUrl(url));
            if (failDownload)
                return BlenderError::DownloadFailed;
            if (url.ends_with(".sha256")) {
                const auto hash = transfer::sha256File(archive);
                if (!hash)
                    return BlenderError::IoError;
                test::writeFile(path, (badChecksum ? std::string(64, '0') : *hash.value) + "  " +
                                          release.archiveName + "\n");
            } else
                std::filesystem::copy_file(archive, path);
            return BlenderError::None;
        };
    }
    void TearDown() override {
        std::error_code ignored;
        for (std::filesystem::recursive_directory_iterator it(directory.path(), ignored), end;
             it != end && !ignored; it.increment(ignored))
            if (!it->is_symlink())
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, ignored);
    }
    BlenderResult<VerifiedBlender> install() {
        return BlenderInstaller(cache, download).ensureVersion("4.2.1");
    }
    RenderRequest request(std::string_view name = "scene.blend") {
        RenderRequest r;
        r.scene = directory.path() / std::string(name);
        test::writeFile(r.scene, "Scene");
        r.outputDirectory = directory.path() / "frames with spaces";
        r.task = {"job", "chunk", 1, 3, 1, "frame_####", "PNG"};
        r.timeout = 2s;
        return r;
    }
};
} // namespace
namespace {
struct ExecutorHarness {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<protocol::Message> messages;
    std::vector<transfer::FileRequest> files;
    std::vector<std::string> cancelledFiles;
    std::unique_ptr<jobs::JobExecutor> executor;
    ExecutorHarness(jobs::ExecutorOptions options, Download download) {
        executor = std::make_unique<jobs::JobExecutor>(
            std::move(options),
            jobs::JobExecutor::Callbacks{[this](const protocol::Message& message) {
                                             const std::lock_guard lock(mutex);
                                             messages.push_back(message);
                                             changed.notify_all();
                                         },
                                         [this](const transfer::FileRequest& file) {
                                             const std::lock_guard lock(mutex);
                                             files.push_back(file);
                                             changed.notify_all();
                                         },
                                         [this](const std::string& id) {
                                             const std::lock_guard lock(mutex);
                                             cancelledFiles.push_back(id);
                                         }},
            std::move(download));
    }
    ~ExecutorHarness() { executor.reset(); }
    void message(protocol::Message message) {
        network::NetworkEvent event{network::EventKind::JobMessage};
        event.message = std::move(message);
        executor->handle(event);
    }
    bool waitMessages(protocol::MessageType type, std::size_t count = 1) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] {
            return std::size_t(std::count_if(messages.begin(), messages.end(), [&](const auto& m) {
                       return protocol::ProtocolCodec::typeOf(m) == type;
                   })) >= count;
        });
    }
    bool waitFiles(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 15s, [&] { return files.size() >= count; });
    }
    bool state(jobs::ExecutorState expected) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        do {
            for (const auto& event : executor->drainEvents())
                if (event.state == expected)
                    return true;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }
    void transfer(const transfer::FileRequest& file, transfer::Direction direction) {
        network::NetworkEvent event{network::EventKind::TransferProgress};
        event.transfer = transfer::TransferProgress{file.fileId,
                                                    direction,
                                                    transfer::TransferState::Completed,
                                                    transfer::TransferError::None,
                                                    0,
                                                    0,
                                                    0,
                                                    file.path};
        executor->handle(event);
    }
    void offer(const std::filesystem::path& project, std::string scene = "scene.blend") {
        auto* zip = zip_open(project.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
        ASSERT_NE(zip, nullptr);
        static const char text[] = "Fixture scene";
        auto* source = zip_source_buffer(zip, text, sizeof(text), 0);
        ASSERT_NE(source, nullptr);
        ASSERT_GE(zip_file_add(zip, scene.c_str(), source, 0), 0);
        ASSERT_EQ(zip_close(zip), 0);
        message(protocol::JobOffer{"executor-job", "Fixture", "CYCLES", 1, 3, 1,
                                   std::filesystem::file_size(project), "4.2.1", "PNG"});
        ASSERT_TRUE(waitMessages(protocol::MessageType::JobAccept));
        transfer({project, "executor-job_PROJECT_ZIP"}, transfer::Direction::Receiving);
    }
};
} // namespace
TEST_F(BlenderTest, ExecutorPackagesActualChildOutputAndCleansOnlyAfterFinalAck) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip");
    const protocol::TaskAssign task{"executor-job", "first_attempt", 1, 2, 1, "frame_####", "PNG"};
    harness.message(task);
    ASSERT_TRUE(harness.waitFiles(1));
    transfer::FileRequest result;
    {
        const std::lock_guard lock(harness.mutex);
        result = harness.files.front();
    }
    ASSERT_TRUE(std::filesystem::exists(result.path));
    EXPECT_EQ(result.fileId, jobs::resultFileId(task.chunkId));
    EXPECT_EQ(
        jobs::ResultAssembler::validate(task, result.path, directory.path() / "validated").size(),
        2u);
    harness.transfer(result, transfer::Direction::Sending);
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Completed));
    EXPECT_FALSE(std::filesystem::exists(result.path.parent_path()));
    // Source reuses the accepted project for a subsequent chunk without another file transfer.
    harness.message(
        protocol::TaskAssign{"executor-job", "second_attempt", 3, 3, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitFiles(2));
    EXPECT_EQ(requests, 2); // one verified installation (archive and published checksum)
}
TEST_F(BlenderTest, ExecutorRetentionPreservesConfirmedResults) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    options.keepCompletedJobs = true;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip");
    harness.message(protocol::TaskAssign{"executor-job", "attempt", 1, 1, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitFiles(1));
    transfer::FileRequest result;
    {
        const std::lock_guard lock(harness.mutex);
        result = harness.files.front();
    }
    harness.transfer(result, transfer::Direction::Sending);
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Completed));
    harness.executor.reset();
    EXPECT_TRUE(std::filesystem::exists(result.path));
    EXPECT_TRUE(std::filesystem::exists(result.path.parent_path() / "frames/frame_0001.png"));
}
TEST_F(BlenderTest, ExecutorRejectsQuotaBeforeInstallingOrRendering) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    options.quotaBytes = 1;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip");
    harness.message(protocol::TaskAssign{"executor-job", "attempt", 1, 1, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitMessages(protocol::MessageType::FileErr));
    EXPECT_EQ(requests, 0);
    const std::lock_guard lock(harness.mutex);
    EXPECT_TRUE(harness.files.empty());
}
TEST_F(BlenderTest, ExecutorPauseResumeAndCancellationNeverSendStaleResults) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip", "sleep.blend");
    harness.message(protocol::TaskAssign{"executor-job", "attempt", 1, 1, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Rendering));
    harness.message(protocol::JobPause{"executor-job"});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Paused));
    harness.message(
        protocol::JobOffer{"queued-job", "Next job", "CYCLES", 1, 1, 1, 0, "4.2.1", "PNG"});
    ASSERT_TRUE(harness.waitMessages(protocol::MessageType::JobAccept, 2));
    harness.message(protocol::JobResume{"executor-job"});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Rendering));
    harness.message(protocol::TaskCancel{"executor-job", "attempt"});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Cancelled));
    const std::lock_guard lock(harness.mutex);
    EXPECT_TRUE(harness.files.empty());
    EXPECT_FALSE(harness.cancelledFiles.empty());
}
TEST_F(BlenderTest, ExecutorReturnsValidPartialFramesWithAnErrorReport) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip", "missing.blend");
    harness.message(protocol::TaskAssign{"executor-job", "attempt", 1, 3, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitFiles(1));
    const std::lock_guard lock(harness.mutex);
    auto* zip = zip_open(harness.files.front().path.string().c_str(), ZIP_RDONLY, nullptr);
    ASSERT_NE(zip, nullptr);
    EXPECT_EQ(zip_get_num_entries(zip, 0), 2);
    EXPECT_GE(zip_name_locate(zip, "blendnet-errors.json", 0), 0);
    EXPECT_GE(zip_name_locate(zip, "frame_0001.png", 0), 0);
    zip_discard(zip);
}
TEST_F(BlenderTest, ExecutorDisconnectInvalidatesTheLeaseAndFreshOfferCanRenderAgain) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip", "sleep.blend");
    harness.message(
        protocol::TaskAssign{"executor-job", "old_attempt", 1, 1, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Rendering));
    harness.executor->handle(network::NetworkEvent{network::EventKind::Reconnecting});
    ASSERT_TRUE(harness.state(jobs::ExecutorState::Cancelled));
    harness.offer(directory.path() / "project.zip");
    harness.message(
        protocol::TaskAssign{"executor-job", "new_attempt", 1, 1, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitFiles(1));
    const std::lock_guard lock(harness.mutex);
    ASSERT_EQ(harness.files.size(), 1u);
    EXPECT_EQ(harness.files.front().fileId, jobs::resultFileId("new_attempt"));
}
TEST_F(BlenderTest, ExecutorRejectsAssignmentOutsideAcceptedRange) {
    jobs::ExecutorOptions options;
    options.directory = directory.path() / "executor";
    options.blenderCache = cache;
    ExecutorHarness harness(options, download);
    harness.offer(directory.path() / "project.zip");
    harness.message(
        protocol::TaskAssign{"executor-job", "forged_attempt", 1, 4, 1, "frame_####", "PNG"});
    ASSERT_TRUE(harness.waitMessages(protocol::MessageType::FileErr));
    EXPECT_EQ(requests, 0);
    const std::lock_guard lock(harness.mutex);
    EXPECT_TRUE(harness.files.empty());
}
TEST(BlenderMetadataTest, OfficialUrlsAndVersionValidation) {
    for (const auto* version : {"", "../4.2.1", "4.2", "4.2.1.exe", "04.2.1", "4.-2.1",
                                "4.2.1/other", "4.2.1\n", "2.80.0"})
        EXPECT_FALSE(officialRelease(version, {System::Linux, Architecture::X64}));
    const auto linuxRelease = officialRelease("4.2.1", {System::Linux, Architecture::X64});
    ASSERT_TRUE(linuxRelease);
    EXPECT_EQ(linuxRelease.value->archiveName, "blender-4.2.1-linux-x64.tar.xz");
    EXPECT_TRUE(officialDownloadUrl(linuxRelease.value->archiveUrl));
    EXPECT_EQ(officialRelease("4.2.1", {System::MacOS, Architecture::Arm64}).value->archiveName,
              "blender-4.2.1-macos-arm64.dmg");
    EXPECT_EQ(officialRelease("4.2.1", {System::Windows, Architecture::X64}).value->archiveName,
              "blender-4.2.1-windows-x64.zip");
    for (const auto* url :
         {"http://download.blender.org/release/Blender4.2/file",
          "https://download.blender.org.evil/release/Blender4.2/file",
          "https://download.blender.org/release/Blender4.2/../file", "https://example.com/file"})
        EXPECT_FALSE(officialDownloadUrl(url));
}
TEST(BlenderMetadataTest, ChecksumSelectionIsExactAndRejectsAmbiguity) {
    const auto hash = std::string(64, 'a');
    EXPECT_EQ(publishedChecksum(hash + " *file.tar.xz\r\n", "file.tar.xz"), hash);
    EXPECT_FALSE(publishedChecksum(hash + "  other-file.tar.xz\n", "file.tar.xz"));
    EXPECT_FALSE(publishedChecksum(hash + " file\n" + hash + " file\n", "file"));
    EXPECT_FALSE(publishedChecksum(std::string(64, 'g') + " file\n", "file"));
}
TEST_F(BlenderTest, InstallationIsVerifiedAndReusedOffline) {
    const auto binary = install();
    ASSERT_TRUE(binary) << toString(binary.error);
    EXPECT_EQ(binary.value->validate(), BlenderError::None);
    EXPECT_EQ(requests, 2);
    failDownload = true;
    const auto again = install();
    ASSERT_TRUE(again);
    EXPECT_EQ(requests, 2);
    EXPECT_EQ(again.value->executable(), binary.value->executable());
    const auto gpu = GpuInfoService::probe(*again.value);
    ASSERT_TRUE(gpu) << gpu.diagnostic;
    EXPECT_EQ(gpu.value->gpu.gpuModel, "Fixture CPU");
    EXPECT_GT(gpu.value->gpu.ramMB, 0);
    EXPECT_EQ(gpu.value->gpu.backend, "CPU");
    EXPECT_EQ(gpu.value->availableBackends.size(), 2);
}
TEST_F(BlenderTest, WrongChecksumDeletesArchiveAndNeverIssuesAnExecutable) {
    badChecksum = true;
    const auto binary = install();
    EXPECT_FALSE(binary);
    EXPECT_EQ(binary.error, BlenderError::ChecksumMismatch);
    EXPECT_FALSE(std::filesystem::exists(cache / release.key));
    EXPECT_FALSE(std::filesystem::exists(cache / "downloads" / release.archiveName));
    badChecksum = false;
    EXPECT_TRUE(install());
}
TEST_F(BlenderTest, DownloadFailureCancellationAndInvalidVersionLeaveNoInstallation) {
    failDownload = true;
    EXPECT_EQ(install().error, BlenderError::DownloadFailed);
    std::atomic_bool cancelled{true};
    EXPECT_EQ(BlenderInstaller(cache, download).ensureVersion("4.2.1", &cancelled).error,
              BlenderError::Cancelled);
    EXPECT_EQ(BlenderInstaller(cache, download).ensureVersion("../4.2.1").error,
              BlenderError::InvalidVersion);
    EXPECT_FALSE(std::filesystem::exists(cache / release.key));
}
TEST_F(BlenderTest, ModifiedExecutableIsRejectedBeforeProbeOrRendering) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    std::filesystem::permissions(binary.value->executable(), std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    {
        std::ofstream append(binary.value->executable(), std::ios::app);
        append << "corrupted";
    }
    EXPECT_EQ(binary.value->validate(), BlenderError::CacheCorrupt);
    EXPECT_EQ(GpuInfoService::probe(*binary.value).error, BlenderError::CacheCorrupt);
    EXPECT_EQ(install().error, BlenderError::CacheCorrupt);
}
TEST_F(BlenderTest, ModifiedManifestAndAddedScriptsInvalidateTheCache) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    std::filesystem::permissions(binary.value->directory(), std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
    const auto added = binary.value->directory() / "unverified.py";
    test::writeFile(added, "raise RuntimeError('must never run')");
    EXPECT_EQ(binary.value->validate(), BlenderError::CacheCorrupt);
    std::filesystem::remove(added);
    EXPECT_EQ(binary.value->validate(), BlenderError::None);
    test::writeFile(cache / release.key / "manifest.json", "malformed manifest");
    EXPECT_EQ(install().error, BlenderError::CacheCorrupt);
}
TEST_F(BlenderTest, ArchiveNamesAndExternalLinksAreRejectedEvenWithAMatchingTestChecksum) {
#ifdef __linux__
    const auto package = directory.path() / "package";
    std::error_code error;
    std::filesystem::create_directory_symlink(directory.path(),
                                              package / release.topDirectory / "escape", error);
    ASSERT_FALSE(error);
    ASSERT_EQ(
        detail::runProcess(
            {"tar", "-cJf", utf8(archive), "-C", utf8(package), release.topDirectory}, nullptr, 10s)
            .error,
        BlenderError::None);
    EXPECT_EQ(install().error, BlenderError::ArchiveRejected);
    EXPECT_FALSE(std::filesystem::exists(cache / release.key));
#else
    GTEST_SKIP() << "Linux tar fixture";
#endif
}
TEST_F(BlenderTest, RenderProcessDrainsPipesAndValidatesAllFrames) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    auto r = request("scene ; no shell.blend");
    std::vector<protocol::Progress> progress;
    const auto result = RenderProcess::run(*binary.value, r, nullptr,
                                           [&](const auto& p) { progress.push_back(p); });
    ASSERT_TRUE(result) << toString(result.error) << result.diagnostic;
    ASSERT_EQ(result.value->frames.size(), 3);
    EXPECT_EQ(result.value->frames[0].width, 1);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().framesDone, 3);
    EXPECT_EQ(RenderProcess::run(*binary.value, r).error, BlenderError::InvalidRequest);
}
TEST_F(BlenderTest, ProcessFailureMissingFramesAndUnavailableBackendAreReported) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    EXPECT_EQ(RenderProcess::run(*binary.value, request("failure.blend")).error,
              BlenderError::ProcessFailed);
    EXPECT_EQ(RenderProcess::run(*binary.value, request("missing.blend")).error,
              BlenderError::MissingFrames);
    auto r = request();
    r.task.frameStart = -1;
    EXPECT_EQ(RenderProcess::run(*binary.value, r).error, BlenderError::InvalidRequest);
    r.task.frameStart = 1;
    r.backend = "METAL";
    EXPECT_EQ(RenderProcess::run(*binary.value, r).error, BlenderError::UnsupportedBackend);
}
TEST_F(BlenderTest, AsyncCancellationAndTimeoutReapTheChild) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    auto r = request("sleep.blend");
    RenderProcess process;
    ASSERT_TRUE(process.start(*binary.value, r));
    EXPECT_FALSE(process.start(*binary.value, r));
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    process.cancel();
    process.wait();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
    EXPECT_EQ(process.snapshot().state, RenderState::Cancelled);
    r.timeout = 100ms;
    EXPECT_EQ(RenderProcess::run(*binary.value, r).error, BlenderError::Timeout);
}
TEST_F(BlenderTest, QuietLongFrameProducesAHeartbeatBeforeItsTimeout) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    auto r = request("sleep.blend");
    r.timeout = 6s;
    int heartbeats = 0;
    const auto result = RenderProcess::run(*binary.value, r, nullptr, [&](const auto& progress) {
        EXPECT_EQ(progress.framesDone, 0);
        ++heartbeats;
    });
    EXPECT_EQ(result.error, BlenderError::Timeout);
    EXPECT_GE(heartbeats, 2);
}
TEST_F(BlenderTest, BenchmarkPersistsScoreWithoutLosingOtherSettings) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    const auto config = directory.path() / "settings.json";
    AppSettings before;
    before.workDirectory = "preserve-work";
    ASSERT_EQ(before.save(config), SettingsError::None);
    const auto result =
        Benchmark::run(*binary.value, "CPU", directory.path() / "benchmark", config);
    ASSERT_TRUE(result);
    EXPECT_GT(result.value->score, 0);
    AppSettings after;
    ASSERT_EQ(AppSettings::load(config, after), SettingsError::None);
    EXPECT_EQ(after.benchmarkScore, result.value->score);
    EXPECT_EQ(after.workDirectory, before.workDirectory);
    EXPECT_EQ(after.computeBackend, "CPU");
}
TEST(BlenderProcessTest, BoundedCaptureDrainsBothStreamsAndCancellationWorksAfterEof) {
    const auto flooded =
        detail::runProcess({BLENDNET_FAKE_BLENDER_PATH, "flood"}, nullptr, 5s, {}, 1024);
    EXPECT_EQ(flooded.error, BlenderError::None);
    EXPECT_TRUE(flooded.truncated);
    EXPECT_LE(flooded.out.size(), 1024);
    EXPECT_LE(flooded.err.size(), 1024);
    std::atomic_bool cancelled{false};
    detail::ProcessOutput output;
    std::jthread worker([&] {
        output = detail::runProcess({BLENDNET_FAKE_BLENDER_PATH, "close-streams"}, &cancelled, 5s);
    });
    std::this_thread::sleep_for(100ms);
    const auto start = std::chrono::steady_clock::now();
    cancelled = true;
    worker.join();
    EXPECT_EQ(output.error, BlenderError::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
}
TEST(BlenderProgressTest, FraIsNotACompletedFrameAndSavedFramesAreDeduplicated) {
    const test::TemporaryDirectory dir;
    RenderProgressParser parser({"job", "chunk", -2, 2, 2, "frame_####", "PNG"}, dir.path());
    const auto started = parser.consume("Fra:-2 | Initializing");
    ASSERT_TRUE(started);
    EXPECT_EQ(started->framesDone, 0);
    EXPECT_FALSE(parser.consume("Fra:1 | not aligned"));
    EXPECT_FALSE(parser.consume("Saved: '/other/frame_0000.png'"));
    const auto saved = "Saved: '" + utf8(dir.path() / "frame_-0002.png") + "'";
    const auto progress = parser.consume(saved);
    ASSERT_TRUE(progress);
    EXPECT_EQ(progress->framesDone, 1);
    EXPECT_FALSE(parser.consume(saved));
    EXPECT_FALSE(parser.consume(std::string(20000, 'x')));
}
