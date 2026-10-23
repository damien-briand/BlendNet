#include "TemporaryDirectory.h"
#include "jobs/EligibilityFilter.h"
#include "jobs/JobFile.h"
#include "jobs/JobOrchestrator.h"
#include "jobs/ResultAssembler.h"

#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <zip.h>
#include <zlib.h>

using namespace blendnet;
using namespace blendnet::jobs;
using namespace std::chrono_literals;
namespace {
Job job(std::string id = "job") {
    Job j;
    j.id = std::move(id);
    j.projectName = "Project";
    j.frameEnd = 6;
    j.chunkSize = 2;
    j.projectZip = "project.zip";
    j.outputDirectory = "output";
    return j;
}
network::ClientInfo client(std::string id = "client") {
    network::ClientInfo c;
    c.id = std::move(id);
    c.gpu = {"GPU", "nvidia", 8192, 32768, "linux", {}, "OPTIX", {}};
    c.preferences = {{}, "AVAILABLE", {}, 1, {}};
    return c;
}
std::vector<ValidatedFrame> frames(int start, int end, int step = 1) {
    std::vector<ValidatedFrame> result;
    for (std::int64_t n = start; n <= end; n += step)
        result.push_back({int(n), frameName(int(n), "PNG"), 1, 1});
    return result;
}
struct Harness {
    std::vector<std::pair<std::string, protocol::Message>> messages;
    std::vector<std::pair<std::string, transfer::FileRequest>> files;
    JobOrchestrator engine;
    Harness()
        : engine(
              {[this](const auto& id, const auto& message) { messages.emplace_back(id, message); },
               [this](const auto& id, const auto& request) { files.emplace_back(id, request); },
               [](const auto&, const auto&, const auto&, const auto&) {},
               [](const auto&, const auto&, bool) {},
               {},
               {}},
              {100ms, 200ms}) {}
    void ready(const std::string& id = "client", const std::string& jobId = "job") {
        engine.connected(client(id));
        engine.message(id, protocol::JobAccept{jobId});
        transfer::TransferProgress p;
        p.fileId = jobId + "_PROJECT_ZIP";
        p.direction = transfer::Direction::Sending;
        p.state = transfer::TransferState::Completed;
        engine.transferred(id, p);
    }
};
} // namespace
TEST(JobSchedulerTest, SteppedNegativeRangesAndRemainderUseFrameCounts) {
    JobScheduler scheduler;
    auto j = job();
    j.frameStart = -5;
    j.frameEnd = 8;
    j.frameStep = 3;
    ASSERT_TRUE(scheduler.publish(j));
    const auto view = scheduler.snapshot().at(0);
    ASSERT_EQ(view.totalFrames, 5);
    ASSERT_EQ(view.chunks.size(), 3);
    EXPECT_EQ(view.chunks[0].frameStart, -5);
    EXPECT_EQ(view.chunks[0].frameEnd, -2);
    EXPECT_EQ(view.chunks[1].frameStart, 1);
    EXPECT_EQ(view.chunks[1].frameEnd, 4);
    EXPECT_EQ(view.chunks[2].frameStart, 7);
    EXPECT_EQ(view.chunks[2].frameEnd, 7);
    EXPECT_EQ(frameName(std::numeric_limits<int>::min(), "PNG"), "frame_-2147483648.png");
    EXPECT_EQ((FrameRange{INT32_MIN, INT32_MAX, INT32_MAX}.count()), 3);
}
TEST(JobSchedulerTest, InvalidJobsAndExcessiveAllocationsAreRejected) {
    JobScheduler scheduler;
    auto j = job();
    j.frameStep = 0;
    EXPECT_FALSE(scheduler.publish(j));
    j = job();
    j.chunkSize = 0;
    EXPECT_FALSE(scheduler.publish(j));
    j = job();
    j.frameEnd = INT32_MAX;
    j.chunkSize = 1;
    EXPECT_FALSE(scheduler.publish(j));
    j = job();
    j.chunkSize = 100001;
    EXPECT_FALSE(scheduler.publish(j));
    j = job();
    ASSERT_TRUE(scheduler.publish(j));
    EXPECT_FALSE(scheduler.publish(j));
}
TEST(EligibilityFilterTest, EngineAvailabilityFrameLimitAndVramAreAllEnforced) {
    auto c = client();
    auto j = job();
    const FrameRange range{1, 9, 2};
    ASSERT_TRUE(EligibilityFilter::isEligible(c, j, range));
    c.preferences.excludedEngines = {"CYCLES"};
    EXPECT_FALSE(EligibilityFilter::isEligible(c, j, range));
    c = client();
    for (const auto* state : {"BUSY_GAME", "BUSY_WORK", "DND", "OFFLINE"}) {
        c.preferences.availability = state;
        EXPECT_FALSE(EligibilityFilter::isEligible(c, j, range));
    }
    c = client();
    c.preferences.maxFramesPerJob = 4;
    EXPECT_FALSE(EligibilityFilter::isEligible(c, j, range));
    c.preferences.maxFramesPerJob = 5;
    EXPECT_TRUE(EligibilityFilter::isEligible(c, j, range));
    j.minimumVramMB = 8193;
    EXPECT_FALSE(EligibilityFilter::isEligible(c, j, range));
}
TEST(EligibilityFilterTest, BackendCompatibilityAndEeveeException) {
    auto c = client();
    auto j = job();
    for (const auto* backend : {"OPTIX", "CUDA", "CPU"}) {
        c.gpu.backend = backend;
        EXPECT_TRUE(EligibilityFilter::isEligible(c, j, {1, 2, 1}));
    }
    for (const auto* backend : {"HIP", "ONEAPI", "METAL", "UNKNOWN"}) {
        c.gpu.backend = backend;
        EXPECT_FALSE(EligibilityFilter::isEligible(c, j, {1, 2, 1}));
    }
    for (const auto& [backend, vendor] :
         {std::pair{"HIP", "amd"}, {"ONEAPI", "intel"}, {"METAL", "apple"}}) {
        c.gpu.backend = backend;
        c.gpu.gpuVendor = vendor;
        c.gpu.os = "macos";
        EXPECT_TRUE(EligibilityFilter::isEligible(c, j, {1, 2, 1}));
    }
    j.engine = "EEVEE";
    c.gpu.backend = "UNKNOWN";
    EXPECT_TRUE(EligibilityFilter::isEligible(c, j, {1, 2, 1}));
}
TEST(JobSchedulerTest, ExclusiveAssignmentsRetryLimitAndLateResults) {
    JobScheduler scheduler;
    ASSERT_TRUE(scheduler.publish(job()));
    ASSERT_TRUE(scheduler.assign("job", 0, "a"));
    EXPECT_FALSE(scheduler.assign("job", 0, "b"));
    EXPECT_FALSE(scheduler.assign("job", 1, "a"));
    const auto first = scheduler.find("job")->chunks[0].assignmentId;
    ASSERT_TRUE(scheduler.fail("job", first, "a"));
    ASSERT_TRUE(scheduler.assign("job", 0, "b"));
    EXPECT_FALSE(scheduler.complete("job", first, "a", frames(1, 2)));
    const auto second = scheduler.find("job")->chunks[0].assignmentId;
    EXPECT_NE(first, second);
    ASSERT_TRUE(scheduler.fail("job", second, "b"));
    ASSERT_TRUE(scheduler.assign("job", 0, "c"));
    ASSERT_TRUE(scheduler.fail("job", scheduler.find("job")->chunks[0].assignmentId, "c"));
    EXPECT_EQ(scheduler.find("job")->chunks[0].state, Chunk::State::Failed);
    EXPECT_EQ(scheduler.find("job")->chunks[0].attempts, 3);
    EXPECT_FALSE(scheduler.assign("job", 0, "d"));
    EXPECT_FALSE(scheduler.finalizeWithFailures("job"));
}
TEST(JobSchedulerTest, ProgressIsNotProofOfCompletionAndAssemblyIsRequired) {
    JobScheduler scheduler;
    auto j = job();
    j.frameEnd = 2;
    ASSERT_TRUE(scheduler.publish(j));
    ASSERT_TRUE(scheduler.assign("job", 0, "a"));
    const auto id = scheduler.find("job")->chunks[0].assignmentId;
    ASSERT_TRUE(scheduler.start("job", id, "a"));
    EXPECT_FALSE(scheduler.progress({"job", id, 2, 2, 2, 100.0, {}}, "b"));
    EXPECT_TRUE(scheduler.progress({"job", id, 2, 2, 2, 100.0, {}}, "a"));
    EXPECT_TRUE(scheduler.progress({"job", id, 2, 2, 2, 100.0, {}}, "a"));
    EXPECT_EQ(scheduler.find("job")->validFrames, 0);
    EXPECT_EQ(scheduler.find("job")->job.state, Job::State::Rendering);
    EXPECT_FALSE(scheduler.complete("job", id, "a", frames(1, 1)));
    ASSERT_TRUE(scheduler.complete("job", id, "a", frames(1, 2)));
    EXPECT_EQ(scheduler.find("job")->job.state, Job::State::Assembling);
    ASSERT_TRUE(scheduler.assembled("job", false));
    EXPECT_EQ(scheduler.find("job")->validFrames, 0);
    ASSERT_TRUE(scheduler.assign("job", 0, "b"));
    const auto retry = scheduler.find("job")->chunks[0].assignmentId;
    ASSERT_TRUE(scheduler.start("job", retry, "b"));
    ASSERT_TRUE(scheduler.complete("job", retry, "b", frames(1, 2)));
    ASSERT_TRUE(scheduler.assembled("job", true));
    EXPECT_EQ(scheduler.find("job")->job.state, Job::State::Done);
}
TEST(JobSchedulerTest, ExplicitFinalizationRequiresAnExhaustedFailureList) {
    JobScheduler scheduler;
    auto j = job();
    j.frameEnd = 4;
    ASSERT_TRUE(scheduler.publish(j));
    ASSERT_TRUE(scheduler.assign("job", 0, "a"));
    const auto first = scheduler.find("job")->chunks[0].assignmentId;
    ASSERT_TRUE(scheduler.start("job", first, "a"));
    ASSERT_TRUE(scheduler.complete("job", first, "a", frames(1, 2)));
    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_TRUE(scheduler.assign("job", 1, "a"));
        ASSERT_TRUE(scheduler.fail("job", scheduler.find("job")->chunks[1].assignmentId, "a"));
    }
    EXPECT_NE(scheduler.find("job")->job.state, Job::State::Done);
    ASSERT_TRUE(scheduler.finalizeWithFailures("job"));
    EXPECT_TRUE(scheduler.find("job")->finalizedWithFailures);
    EXPECT_EQ(scheduler.find("job")->failures.size(), 1);
    EXPECT_EQ(scheduler.find("job")->job.state, Job::State::Assembling);
    ASSERT_TRUE(scheduler.assembled("job", true));
    EXPECT_EQ(scheduler.find("job")->job.state, Job::State::Done);
    EXPECT_EQ(scheduler.find("job")->validFrames, 2);
}
TEST(JobOrchestratorTest, AcceptanceAndProjectAckPrecedeAssignmentAndProjectIsSentOnce) {
    Harness h;
    ASSERT_TRUE(h.engine.publish(job()));
    h.engine.connected(client());
    ASSERT_EQ(h.messages.size(), 1);
    ASSERT_TRUE(std::holds_alternative<protocol::JobOffer>(h.messages[0].second));
    h.engine.message("client", protocol::JobAccept{"job"});
    ASSERT_EQ(h.files.size(), 1);
    ASSERT_EQ(h.messages.size(), 1);
    transfer::TransferProgress p;
    p.fileId = "job_PROJECT_ZIP";
    p.direction = transfer::Direction::Sending;
    p.state = transfer::TransferState::Completed;
    h.engine.transferred("client", p);
    ASSERT_EQ(h.messages.size(), 2);
    ASSERT_TRUE(std::holds_alternative<protocol::TaskAssign>(h.messages.back().second));
    const auto task = std::get<protocol::TaskAssign>(h.messages.back().second);
    h.engine.validated("job", task.chunkId, "client", frames(1, 2));
    EXPECT_EQ(h.files.size(), 1);
    EXPECT_EQ(h.messages.size(), 3);
}
TEST(JobOrchestratorTest, DeclineAndOfferTimeoutDoNotBanFutureJobs) {
    Harness h;
    h.engine.connected(client());
    ASSERT_TRUE(h.engine.publish(job()));
    h.engine.message("client", protocol::JobDecline{"job"});
    EXPECT_TRUE(h.files.empty());
    ASSERT_TRUE(h.engine.publish(job("later")));
    EXPECT_EQ(h.messages.size(), 2);
    h.engine.tick(JobOrchestrator::Clock::now() + 150ms);
    h.engine.message("client", protocol::JobAccept{"later"});
    EXPECT_TRUE(h.files.empty());
}
TEST(JobOrchestratorTest,
     TimeoutRedistributesAndPauseDefersTimeoutAndCancellationInvalidatesResults) {
    Harness h;
    ASSERT_TRUE(h.engine.publish(job()));
    h.ready("a");
    auto view = h.engine.snapshot().at(0);
    const auto id = view.chunks[0].assignmentId;
    h.engine.pause("job");
    h.engine.tick(JobOrchestrator::Clock::now() + 1s);
    EXPECT_EQ(h.engine.snapshot().at(0).chunks[0].attempts, 0);
    h.engine.resume("job");
    h.engine.tick(JobOrchestrator::Clock::now() + 1s);
    EXPECT_EQ(h.engine.snapshot().at(0).chunks[0].attempts, 1);
    h.ready("b");
    h.engine.validated("job", id, "a", frames(1, 2));
    EXPECT_EQ(h.engine.snapshot().at(0).validFrames, 0);
    h.engine.cancel("job");
    const auto current = h.engine.snapshot().at(0).chunks[0].assignmentId;
    h.engine.validated("job", current, "b", frames(1, 2));
    EXPECT_EQ(h.engine.snapshot().at(0).job.state, Job::State::Cancelled);
}
TEST(ResultAssemblerTest, ValidImagesAssembleAndMissingFramesPreventCompletion) {
    const test::TemporaryDirectory dir;
    auto j = job();
    j.frameEnd = 2;
    j.outputDirectory = dir.path() / "out";
    protocol::TaskAssign task{"job", "task", 1, 2, 1, "frame_####", "PNG"};
    const auto archive = dir.path() / "result.zip";
    ASSERT_TRUE(ResultAssembler::simulatedResult(task, archive));
    const auto validated = ResultAssembler::validate(task, archive, dir.path() / "stage");
    ASSERT_EQ(validated.size(), 2);
    std::map<int, ValidatedFrame> images;
    for (const auto& f : validated)
        images.emplace(f.frame, f);
    ASSERT_TRUE(ResultAssembler::assemble(j, images));
    EXPECT_TRUE(std::filesystem::exists(j.outputDirectory / "frame_0002.png"));
    images.erase(2);
    EXPECT_FALSE(ResultAssembler::assemble(j, images));
    j.outputDirectory = dir.path() / "partial";
    EXPECT_TRUE(ResultAssembler::assemble(j, images, nullptr, true));
    EXPECT_TRUE(std::filesystem::exists(j.outputDirectory / "frame_0001.png"));
    EXPECT_FALSE(std::filesystem::exists(j.outputDirectory / "frame_0002.png"));
}
TEST(ResultAssemblerTest, TraversalUnexpectedNamesSymlinksAndMissingEntriesAreRejected) {
    const test::TemporaryDirectory dir;
    protocol::TaskAssign task{"job", "task", 1, 1, 1, "frame_####", "PNG"};
    const auto archive = dir.path() / "result.zip";
    ASSERT_TRUE(ResultAssembler::simulatedResult(task, archive));
    zip_t* zip = zip_open(archive.string().c_str(), 0, nullptr);
    ASSERT_NE(zip, nullptr);
    ASSERT_EQ(zip_file_rename(zip, 0, "../outside.png", 0), 0);
    ASSERT_EQ(zip_close(zip), 0);
    EXPECT_TRUE(ResultAssembler::validate(task, archive, dir.path() / "stage").empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path().parent_path() / "outside.png"));
    ASSERT_TRUE(ResultAssembler::simulatedResult(task, archive));
    zip = zip_open(archive.string().c_str(), 0, nullptr);
    ASSERT_NE(zip, nullptr);
    ASSERT_EQ(zip_file_set_external_attributes(zip, 0, 0, ZIP_OPSYS_UNIX, 0120777u << 16), 0);
    ASSERT_EQ(zip_close(zip), 0);
    EXPECT_TRUE(ResultAssembler::validate(task, archive, dir.path() / "stage2").empty());
    ASSERT_TRUE(ResultAssembler::simulatedResult(task, archive));
    task.frameEnd = 2;
    EXPECT_TRUE(ResultAssembler::validate(task, archive, dir.path() / "stage3").empty());
}
TEST(ResultAssemblerTest, InconsistentDimensionsAndPngCrcAreRejectedAndStagingIsCleaned) {
    const test::TemporaryDirectory dir;
    const auto archive = dir.path() / "result.zip";
    protocol::TaskAssign task{"job", "task", 1, 1, 1, "frame_####", "PNG"};
    ASSERT_TRUE(ResultAssembler::simulatedResult(task, archive));
    const auto original = ResultAssembler::validate(task, archive, dir.path() / "original");
    ASSERT_EQ(original.size(), 1);
    std::ifstream input(original[0].path, std::ios::binary);
    const std::vector<unsigned char> png{std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>()};
    auto second = png;
    second[19] = 2;
    const auto crc = crc32(0, second.data() + 12, 17);
    for (int i = 0; i < 4; ++i)
        second[29 + i] = (crc >> (24 - 8 * i)) & 255;
    zip_t* zip = zip_open(archive.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    ASSERT_NE(zip, nullptr);
    auto* one = zip_source_buffer(zip, png.data(), png.size(), 0);
    auto* two = zip_source_buffer(zip, second.data(), second.size(), 0);
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    ASSERT_GE(zip_file_add(zip, "frame_0001.png", one, 0), 0);
    ASSERT_GE(zip_file_add(zip, "frame_0002.png", two, 0), 0);
    ASSERT_EQ(zip_close(zip), 0);
    task.frameEnd = 2;
    EXPECT_TRUE(ResultAssembler::validate(task, archive, dir.path() / "mismatch").empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "mismatch"));
    second = png;
    second[29] ^= 1;
    zip = zip_open(archive.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    ASSERT_NE(zip, nullptr);
    one = zip_source_buffer(zip, second.data(), second.size(), 0);
    ASSERT_NE(one, nullptr);
    ASSERT_GE(zip_file_add(zip, "frame_0001.png", one, 0), 0);
    ASSERT_EQ(zip_close(zip), 0);
    task.frameEnd = 1;
    EXPECT_TRUE(ResultAssembler::validate(task, archive, dir.path() / "bad-crc").empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "bad-crc"));
}
TEST(JobFileTest, RelativePathsAndStrictIntegerValidation) {
    const test::TemporaryDirectory dir;
    const auto path = dir.path() / "job.json";
    {
        std::ofstream out(path);
        out << R"({"id":"job","projectName":"demo","projectZip":"project.zip","outputDirectory":"out","frameStep":1.5})";
    }
    EXPECT_FALSE(loadJob(path));
    {
        std::ofstream out(path);
        out << R"({"id":"job","projectName":"demo","projectZip":"project.zip","outputDirectory":"out","frameStep":2})";
    }
    const auto loaded = loadJob(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->projectZip, dir.path() / "project.zip");
    EXPECT_EQ(loaded->frameStep, 2);
}
