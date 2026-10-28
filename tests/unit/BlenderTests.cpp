#include "TemporaryDirectory.h"
#include "blender/Benchmark.h"
#include "blender/BlenderInstaller.h"
#include "blender/GpuInfoService.h"
#include "blender/Process.h"
#include "blender/RenderProcess.h"
#include "jobs/JobExecutor.h"
#include "jobs/ResultAssembler.h"
#include "projects/ProjectLibrary.h"
#include "protocol/ProtocolCodec.h"
#include "transfer/TransferEngine.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
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
    ::testing::AssertionResult waitFiles(std::size_t count) {
        std::unique_lock lock(mutex);
        if (changed.wait_for(lock, 15s, [&] { return files.size() >= count; }))
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << diagnostics();
    }
    ::testing::AssertionResult state(jobs::ExecutorState expected) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        std::ostringstream text;
        do {
            for (const auto& event : executor->drainEvents()) {
                if (event.state == expected)
                    return ::testing::AssertionSuccess();
                text << jobs::toString(event.state) << ": " << event.detail << '\n';
                if (event.state == jobs::ExecutorState::Failed)
                    return ::testing::AssertionFailure() << text.str();
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < end);
        return ::testing::AssertionFailure() << "Timed out waiting for "
                                             << jobs::toString(expected) << '\n' << text.str();
    }
    std::string diagnostics() {
        std::ostringstream text;
        for (const auto& event : executor->drainEvents())
            text << jobs::toString(event.state) << ": " << event.detail << '\n';
        return text.str();
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
    // Exercise a normal Windows user directory depth without requiring long-path support.
    if (options.directory.native().size() < 90)
        options.directory /= std::string(90 - options.directory.native().size(), 'w');
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
    EXPECT_LT(result.path.native().size(), 260u);
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
TEST_F(BlenderTest, ProjectMetadataAndPortableArchivePreserveOnlySelectedDependencies) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    const auto root = directory.path() / "project";
    std::filesystem::create_directories(root / "textures");
    test::writeFile(root / "scene.blend", "Original scene, do not modify");
    test::writeFile(root / "textures/image.bin", "texture");
    test::writeFile(root / "private-not-a-dependency.txt", "not shared");
    auto info = projects::ProjectLibrary::inspect(*binary.value, root / "scene.blend");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value->engine, "CYCLES");
    EXPECT_EQ(info.value->end, 3);
    info.value->dependencies = {root / "textures"};
    const auto zip = directory.path() / "export/project.zip";
    ASSERT_EQ(projects::ProjectLibrary::package(*binary.value, *info.value, zip),
              BlenderError::None);
    auto* packaged = zip_open(zip.string().c_str(), ZIP_RDONLY, nullptr);
    ASSERT_NE(packaged, nullptr);
    EXPECT_EQ(zip_get_num_entries(packaged, 0), 3);
    EXPECT_GE(zip_name_locate(packaged, "textures/image.bin", 0), 0);
    EXPECT_LT(zip_name_locate(packaged, "textures\\image.bin", 0), 0);
    zip_discard(packaged);
    const auto output = directory.path() / "unpacked";
    ASSERT_TRUE(archive::ZipService::extractProject(zip, output));
    EXPECT_EQ(test::readFile(output / "textures/image.bin"), "texture");
    EXPECT_FALSE(std::filesystem::exists(output / "private-not-a-dependency.txt"));
    EXPECT_EQ(test::readFile(root / "scene.blend"), "Original scene, do not modify");
    info.value->dependencies = {archive};
    EXPECT_EQ(
        projects::ProjectLibrary::package(*binary.value, *info.value, directory.path() / "bad.zip"),
        BlenderError::InvalidRequest);
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "bad.zip"));
}
TEST(BlenderProjectIntegrationTest, EssentialsWeakReferencesAllowPackagingAndRendering) {
    const auto* cache = std::getenv("BLENDNET_REAL_BLENDER_CACHE");
    const auto* version = std::getenv("BLENDNET_REAL_BLENDER_VERSION");
    if (!cache || !version)
        GTEST_SKIP() << "Set BLENDNET_REAL_BLENDER_CACHE and BLENDNET_REAL_BLENDER_VERSION "
                        "to test with a verified Blender 4.3+ installation";
    const auto binary = BlenderInstaller(std::filesystem::path(cache)).ensureVersion(version);
    ASSERT_TRUE(binary) << toString(binary.error);
    test::TemporaryDirectory directory;
    const auto project = directory.path() / "project";
    std::filesystem::create_directories(project);
    const auto scene = project / "scene.blend";
    const auto script = R"PY(
import bpy, sys
s=bpy.context.scene
s.render.engine='CYCLES'
s.cycles.samples=1
s.cycles.use_denoising=False
s.render.resolution_x=s.render.resolution_y=32
s.render.resolution_percentage=100
s.render.image_settings.file_format='PNG'
s.frame_start=s.frame_end=1
bpy.ops.wm.save_as_mainfile(filepath=sys.argv[sys.argv.index('--')+1])
paths=bpy.utils.blend_paths(absolute=True, packed=False, local=False)
print('ESSENTIALS_REFERENCE:' + str(any('essentials_brushes-' in p for p in paths)), flush=True)
)PY";
    const auto created = detail::runBlender(
        *binary.value,
        {"-b", "--python-exit-code", "72", "--python-expr", script, "--", utf8(scene)},
        nullptr, 60s);
    ASSERT_EQ(created.error, BlenderError::None) << created.out << created.err;
    ASSERT_NE(created.out.find("ESSENTIALS_REFERENCE:True"), std::string::npos)
        << "This regression requires Blender's Essentials brush weak references";
    const auto originalHash = transfer::sha256File(scene);
    ASSERT_TRUE(originalHash);
    const auto info = projects::ProjectLibrary::inspect(*binary.value, scene);
    ASSERT_TRUE(info) << toString(info.error);
    EXPECT_TRUE(info.value->dependencies.empty());
    const auto zip = directory.path() / "export/project.zip";
    ASSERT_EQ(projects::ProjectLibrary::package(*binary.value, *info.value, zip),
              BlenderError::None);
    const auto unpacked = directory.path() / "client";
    ASSERT_TRUE(archive::ZipService::extractProject(zip, unpacked));
    EXPECT_EQ(transfer::sha256File(scene).value, originalHash.value);
    const auto portable = projects::ProjectLibrary::inspect(*binary.value, unpacked / "scene.blend");
    ASSERT_TRUE(portable);
    EXPECT_TRUE(portable.value->dependencies.empty());
    RenderRequest request;
    request.scene = unpacked / "scene.blend";
    request.outputDirectory = directory.path() / "frames";
    request.task = {"essentials", "chunk", 1, 1, 1, "frame_####", "PNG"};
    request.timeout = 60s;
    const auto rendered = RenderProcess::run(*binary.value, request);
    ASSERT_TRUE(rendered) << toString(rendered.error) << rendered.diagnostic;
    EXPECT_EQ(rendered.value->frames.size(), 1u);

    // A real linked library with the same suffix must still be checked and rejected
    // outside the project. The weak-reference exemption cannot hide linked data.
    const auto linked = project / "linked.blend";
    const auto library = directory.path() /
                         "external/5.2/datafiles/assets/brushes/essentials_brushes-gp_draw.blend";
    std::filesystem::create_directories(library.parent_path());
    const auto linkScript = R"PY(
import bpy, sys
args=sys.argv[sys.argv.index('--')+1:]
asset=args[1]
bpy.ops.wm.save_as_mainfile(filepath=asset)
bpy.ops.wm.read_factory_settings()
bpy.data.objects.remove(bpy.data.objects['Cube'], do_unlink=True)
with bpy.data.libraries.load(asset, link=True) as (source, target):
    target.objects=['Cube']
bpy.context.scene.collection.objects.link(target.objects[0])
bpy.ops.wm.save_as_mainfile(filepath=args[0])
)PY";
    const auto loaded = detail::runBlender(
        *binary.value,
        {"-b", "--python-exit-code", "72", "--python-expr", linkScript, "--", utf8(linked),
         utf8(library)},
        nullptr, 60s);
    ASSERT_EQ(loaded.error, BlenderError::None) << loaded.out << loaded.err;
    const auto linkedInfo = projects::ProjectLibrary::inspect(*binary.value, linked);
    ASSERT_TRUE(linkedInfo);
    EXPECT_NE(std::find(linkedInfo.value->dependencies.begin(), linkedInfo.value->dependencies.end(),
                        library),
              linkedInfo.value->dependencies.end());
    EXPECT_EQ(projects::ProjectLibrary::package(*binary.value, *linkedInfo.value,
                                              directory.path() / "linked.zip"),
              BlenderError::InvalidRequest);
}
TEST_F(BlenderTest, InstallationIsVerifiedAndReusedOffline) {
    const auto binary = install();
    ASSERT_TRUE(binary) << toString(binary.error) << ": " << binary.diagnostic;
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
TEST_F(BlenderTest, CachedVersionRequiresAnExactStringAndAcceptsABoundedStringView) {
    const auto binary = install();
    ASSERT_TRUE(binary);
    failDownload = true;
    const auto manifest = cache / release.key / "manifest.json";
    const auto original = nlohmann::json::parse(test::readFile(manifest));
    const std::string versionWithSuffix = "4.2.1.trailing";
    const std::string_view version(versionWithSuffix.data(), 5);
    ASSERT_TRUE(BlenderInstaller(cache, download).ensureVersion(version));
    for (const auto& invalid :
         {nlohmann::json(nullptr), nlohmann::json(421), nlohmann::json(true),
          nlohmann::json::array({"4.2.1"}), nlohmann::json::object({{"value", "4.2.1"}}),
          nlohmann::json("4.2.0"), nlohmann::json("4.2.1.trailing")}) {
        SCOPED_TRACE(invalid.dump());
        auto changed = original;
        changed["version"] = invalid;
        test::writeFile(manifest, changed.dump());
        EXPECT_EQ(BlenderInstaller(cache, download).ensureVersion(version).error,
                  BlenderError::CacheCorrupt);
    }
    test::writeFile(manifest, original.dump());
    EXPECT_TRUE(BlenderInstaller(cache, download).ensureVersion(version));
    EXPECT_EQ(requests, 2);
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
TEST_F(BlenderTest, ExplicitLocalBlenderRunsProbeAndBenchmarkWithoutDownloadingOrWritingItsInstallation) {
    const auto installed = directory.path() / std::filesystem::path(u8"Installed Blender é") / "blender.exe";
    std::filesystem::create_directories(installed.parent_path());
    std::filesystem::copy_file(BLENDNET_FAKE_BLENDER_PATH, installed);
    test::writeFile(installed.parent_path() / "keep.txt", "untouched");
    std::filesystem::permissions(installed.parent_path(), std::filesystem::perms::owner_read |
                                                          std::filesystem::perms::owner_exec);
    const auto binary = BlenderInstaller(cache, download, installed).ensureVersion("4.2.1");
    ASSERT_TRUE(binary) << binary.diagnostic;
    EXPECT_EQ(binary.value->executable(), installed);
    EXPECT_TRUE(GpuInfoService::probe(*binary.value, "CPU"));
    const auto config = directory.path() / "local-settings.json";
    AppSettings settings;
    settings.blenderExecutable = installed;
    ASSERT_EQ(settings.save(config), SettingsError::None);
    const auto benchmark = Benchmark::run(*binary.value, "CPU", directory.path() / "benchmarks", config);
    ASSERT_TRUE(benchmark) << benchmark.diagnostic;
    EXPECT_EQ(requests, 0);
    EXPECT_FALSE(std::filesystem::exists(installed.parent_path() / "runtime"));
    EXPECT_EQ(test::readFile(installed.parent_path() / "keep.txt"), "untouched");
    std::filesystem::permissions(installed.parent_path(), std::filesystem::perms::owner_all);
    std::ofstream(installed, std::ios::app | std::ios::binary) << "modified";
    EXPECT_EQ(binary.value->validate(), BlenderError::CacheCorrupt);
}
TEST_F(BlenderTest, DifferentLocalVersionUsesTheExactManagedVersionRequestedByTheServer) {
    const auto installed = directory.path() / "wrong-version.exe";
    std::filesystem::copy_file(BLENDNET_FAKE_BLENDER_PATH, installed);
    const auto binary = BlenderInstaller(cache, download, installed).ensureVersion("4.2.1");
    ASSERT_TRUE(binary) << binary.diagnostic;
    EXPECT_NE(binary.value->executable(), installed);
    EXPECT_EQ(binary.value->version(), "4.2.1");
    EXPECT_EQ(requests, 2);
}
TEST_F(BlenderTest, MissingSelectedExecutableHasAnActionableErrorWithoutDownloading) {
    const auto installed = directory.path() / "missing-blender.exe";
    const auto binary = BlenderInstaller(cache, download, installed).ensureVersion("4.2.1");
    EXPECT_FALSE(binary);
    EXPECT_EQ(binary.error, BlenderError::InvalidRequest);
    EXPECT_NE(binary.diagnostic.find("missing-blender.exe"), std::string::npos);
    EXPECT_EQ(requests, 0);
}
TEST(BlenderProcessTest, MissingChildIncludesTheOperatingSystemErrorAndExecutable) {
    test::TemporaryDirectory directory;
    const auto child = directory.path() / "missing-child.exe";
    const auto output = detail::runProcess({utf8(child)}, nullptr, 1s);
    EXPECT_EQ(output.error, BlenderError::ProcessFailed);
    EXPECT_NE(detail::processDiagnostic(output).find("missing-child.exe"), std::string::npos);
    EXPECT_NE(detail::processDiagnostic(output).find("exit="), std::string::npos);
}
#ifdef _WIN32
TEST(BlenderProcessTest, WindowsOverridesInheritedEnvironmentWithoutDroppingSystemVariables) {
    const auto output = detail::runProcess({BLENDNET_FAKE_BLENDER_PATH, "environment"}, nullptr,
                                          5s, {}, 4096, {"TEMP=blendnet-temporary-override"});
    ASSERT_EQ(output.error, BlenderError::None) << detail::processDiagnostic(output);
    EXPECT_NE(output.out.find("TEMP=blendnet-temporary-override"), std::string::npos);
    const auto* root = std::getenv("SystemRoot");
    ASSERT_NE(root, nullptr);
    EXPECT_NE(output.out.find("SYSTEM_ROOT=" + std::string(root)), std::string::npos);
}
TEST(BlenderProcessTest, RelocatedChildFindsLibrariesBundledBesideBlendNet) {
    test::TemporaryDirectory directory;
    const auto child = directory.path() / "child.exe";
    std::filesystem::copy_file(BLENDNET_RUNTIME_CHILD_PATH, child);
    // The required fixture DLL stays beside blendnet-tests, outside the child's directory.
    const auto output = detail::runProcess({utf8(child)}, nullptr, 5s);
    EXPECT_EQ(output.error, BlenderError::None) << detail::processDiagnostic(output);
    EXPECT_EQ(output.exitCode, 0);
}
#endif
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
