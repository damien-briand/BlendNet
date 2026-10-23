#include "RenderServer.h"
#include "SourceConnection.h"
#include "TemporaryDirectory.h"
#include "jobs/ResultAssembler.h"
#include "jobs/SimulatedWorker.h"
#include "network/Security.h"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <thread>

using namespace blendnet;
using namespace blendnet::jobs;
using namespace blendnet::network;
using namespace std::chrono_literals;
namespace {
class JobIntegration : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    std::ostringstream logs;
    Logger logger{logs};
    RenderServer server{logger};
    ServerIdentity identity;
    ServerOptions options;
    struct Node {
        SourceConnection connection;
        std::unique_ptr<SimulatedWorker> worker;
        bool corrupt = false;
        Node(Logger& logger) : connection(logger) {}
    };
    std::vector<std::unique_ptr<Node>> nodes;
    Job job;
    void SetUp() override {
        ASSERT_EQ(ensureServerIdentity(directory.path() / "identity", identity),
                  NetworkError::None);
        options.port = 0;
        options.token = identity.token;
        options.certificateFile = identity.certificateFile;
        options.privateKeyFile = identity.privateKeyFile;
        options.transfers.directory = directory.path() / "received";
        options.jobTimings = {500ms, 500ms};
        job.id = "job";
        job.projectName = "demo";
        job.frameEnd = 12;
        job.chunkSize = 4;
        job.projectZip = directory.path() / "project.zip";
        job.outputDirectory = directory.path() / "output";
        ASSERT_TRUE(ResultAssembler::simulatedResult(
            {"job", "fixture", 1, 1, 1, "frame_####", "PNG"}, job.projectZip));
    }
    void TearDown() override {
        for (auto& node : nodes) {
            node->worker.reset();
            node->connection.stop();
        }
        server.stop();
        EXPECT_EQ(logs.str().find(identity.token), std::string::npos);
    }
    void start() { ASSERT_EQ(server.start(options), NetworkError::None); }
    void add(bool corrupt = false, bool simulated = true) {
        const auto root = directory.path() / ("client-" + std::to_string(nodes.size()));
        auto node = std::make_unique<Node>(logger);
        node->corrupt = corrupt;
        ClientOptions client;
        client.source = {"127.0.0.1", server.snapshot().port, identity.token, {}};
        client.machineName = "client-" + std::to_string(nodes.size());
        client.settingsPath = root / "settings.json";
        client.transfers.directory = root / "received";
        ASSERT_EQ(node->connection.start(client), NetworkError::None);
        if (simulated)
            node->worker = std::make_unique<SimulatedWorker>(
                root / "simulation", 30ms,
                [p = node.get()](const auto& message) { p->connection.sendJobMessage(message); },
                [p = node.get()](const auto& file) { p->connection.sendFile(file); });
        nodes.push_back(std::move(node));
    }
    void pump() {
        for (auto& node : nodes)
            for (const auto& event : node->connection.drainEvents()) {
                if (node->worker)
                    node->worker->handle(event);
                if (!node->corrupt || !event.message)
                    continue;
                if (const auto* offer = std::get_if<protocol::JobOffer>(&*event.message))
                    node->connection.sendJobMessage(protocol::JobAccept{offer->jobId});
                if (const auto* task = std::get_if<protocol::TaskAssign>(&*event.message)) {
                    const auto archive = directory.path() / (task->chunkId + ".zip");
                    const protocol::TaskAssign wrong{task->jobId, task->chunkId, 100,  103,
                                                     1,           "frame_####",  "PNG"};
                    if (!ResultAssembler::simulatedResult(wrong, archive))
                        throw std::runtime_error("Fixture failed");
                    node->connection.sendJobMessage(protocol::Progress{
                        task->jobId, task->chunkId, task->frameEnd, 4, 4, 100.0, {}});
                    node->connection.sendFile({archive, resultFileId(task->chunkId), "RESULT_ZIP"});
                }
            }
    }
    template <typename Predicate>
    bool wait(Predicate predicate, std::chrono::milliseconds duration = 10000ms) {
        const auto end = std::chrono::steady_clock::now() + duration;
        do {
            pump();
            if (predicate())
                return true;
            std::this_thread::sleep_for(2ms);
        } while (std::chrono::steady_clock::now() < end);
        return predicate();
    }
    JobSnapshot view() { return server.snapshot().jobs.at(0); }
};
} // namespace
TEST_F(JobIntegration, PausePreservesLeaseAndIgnoresTaskTimeoutThenResumeCompletes) {
    options.jobTimings.taskTimeout = 150ms;
    start();
    add();
    ASSERT_TRUE(wait([this] { return server.snapshot().clients.size() == 1; }));
    server.publishJob(job);
    ASSERT_TRUE(wait(
        [this] { return !server.snapshot().jobs.empty() && view().chunks[0].framesDone >= 1; }));
    const auto assignment = view().chunks[0].assignmentId;
    server.pauseJob(job.id);
    ASSERT_TRUE(wait([this] { return view().job.state == Job::State::Paused; }));
    EXPECT_FALSE(wait([] { return false; }, 400ms));
    EXPECT_EQ(view().chunks[0].attempts, 0);
    EXPECT_EQ(view().chunks[0].assignmentId, assignment);
    server.resumeJob(job.id);
    ASSERT_TRUE(wait([this] { return view().job.state == Job::State::Done; }));
    EXPECT_EQ(view().validFrames, 12);
    EXPECT_FALSE(view().finalizedWithFailures);
}
TEST_F(JobIntegration, CancelledJobCannotBecomeDoneFromLateResults) {
    start();
    add();
    ASSERT_TRUE(wait([this] { return server.snapshot().clients.size() == 1; }));
    server.publishJob(job);
    ASSERT_TRUE(wait(
        [this] { return !server.snapshot().jobs.empty() && view().chunks[0].framesDone >= 1; }));
    server.cancelJob(job.id);
    ASSERT_TRUE(wait([this] { return view().job.state == Job::State::Cancelled; }));
    EXPECT_FALSE(wait([] { return false; }, 300ms));
    EXPECT_EQ(view().validFrames, 0);
    EXPECT_EQ(nodes[0]->connection.snapshot().state, ConnectionState::Ready);
}
TEST_F(JobIntegration, SourceRendererFallbackUsesTheSameResultValidation) {
    options.localRenderer = [](const Job&, const protocol::TaskAssign& task,
                               const std::filesystem::path& result) {
        return ResultAssembler::simulatedResult(task, result);
    };
    start();
    server.publishJob(job);
    ASSERT_TRUE(wait([this] {
        return !server.snapshot().jobs.empty() && view().job.state == Job::State::Done;
    }));
    EXPECT_EQ(view().validFrames, 12);
    EXPECT_TRUE(std::filesystem::exists(job.outputDirectory / "frame_0012.png"));
}
TEST_F(JobIntegration, FalseHundredPercentAndInvalidResultsExhaustRetriesWithoutCompletingJob) {
    job.frameEnd = 4;
    start();
    add(true, false);
    add(true, false);
    add(true, false);
    ASSERT_TRUE(wait([this] { return server.snapshot().clients.size() == 3; }));
    server.publishJob(job);
    ASSERT_TRUE(
        wait([this] { return !server.snapshot().jobs.empty() && !view().failures.empty(); }));
    EXPECT_EQ(view().validFrames, 0);
    EXPECT_EQ(view().chunks[0].attempts, 3);
    EXPECT_NE(view().job.state, Job::State::Done);
    server.finalizeJobWithFailures(job.id);
    ASSERT_TRUE(wait([this] { return view().job.state == Job::State::Done; }));
    EXPECT_TRUE(view().finalizedWithFailures);
    EXPECT_EQ(view().failures.size(), 1);
}
