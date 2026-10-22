#include "TemporaryDirectory.h"
#include "transfer/TransferEngine.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>

using namespace blendnet;
using namespace blendnet::transfer;
namespace {
void generate(const std::filesystem::path& path, std::size_t bytes) {
    std::ofstream output(path, std::ios::binary);
    std::string block(4096, 'x');
    while (bytes) {
        const auto count = std::min(bytes, block.size());
        output.write(block.data(), static_cast<std::streamsize>(count));
        bytes -= count;
    }
    if (!output)
        throw std::runtime_error("Fixture write failed");
}
void finish(TransferSender& sender, TransferReceiver& receiver) {
    int iterations = 0;
    while (sender.progress().state != TransferState::Completed && iterations++ < 10000) {
        const auto message = sender.next();
        ASSERT_TRUE(message);
        const auto reply = receiver.receive(*message);
        if (reply) {
            ASSERT_TRUE(std::holds_alternative<protocol::FileAck>(*reply));
            ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*reply)), TransferError::None);
        }
    }
    ASSERT_EQ(sender.progress().state, TransferState::Completed);
    ASSERT_EQ(receiver.progress().state, TransferState::Completed);
}
} // namespace
TEST(TransferTest, Sha256KnownVectorsAndCancellation) {
    const test::TemporaryDirectory directory;
    const auto file = directory.path() / "file";
    test::writeFile(file, "abc");
    auto hash = sha256File(file);
    ASSERT_TRUE(hash);
    EXPECT_EQ(*hash.value, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    test::writeFile(file, "");
    hash = sha256File(file);
    ASSERT_TRUE(hash);
    EXPECT_EQ(*hash.value, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::atomic_bool cancelled{true};
    EXPECT_EQ(sha256File(file, &cancelled).error, TransferError::Aborted);
    EXPECT_EQ(sha256File(directory.path() / "missing").error, TransferError::IoError);
}
TEST(TransferTest, StreamingRoundTripIncludesPartialLastBlockAndEmptyFile) {
    for (const std::size_t size : {0U, 1U, 2U * 1024 * 1024 + 123U}) {
        const test::TemporaryDirectory directory;
        const auto file = directory.path() / "source.zip";
        generate(file, size);
        TransferSender sender;
        const auto offer = sender.prepare({file, "job_PROJECT_ZIP"});
        ASSERT_TRUE(offer);
        TransferReceiver receiver({directory.path() / "received", size}, "peer");
        const auto ack = receiver.receive(*offer.value);
        ASSERT_TRUE(ack);
        ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*ack)), TransferError::None);
        finish(sender, receiver);
        EXPECT_EQ(std::filesystem::file_size(receiver.progress().path), size);
        EXPECT_EQ(sha256File(receiver.progress().path).value, sha256File(file).value);
        EXPECT_EQ(receiver.progress().path.extension(), ".zip");
    }
}
TEST(TransferTest, RestartResumesOnlyWholeBlocksAndCompletedOffersAreIdempotent) {
    const test::TemporaryDirectory directory;
    const auto file = directory.path() / "source.zip";
    generate(file, 20 * 65536 + 13);
    const FileRequest request{file, "job_PROJECT_ZIP", "PROJECT_ZIP", 65536};
    const TransferOptions options{directory.path() / "received", {}};
    {
        TransferSender sender;
        const auto offer = sender.prepare(request);
        ASSERT_TRUE(offer);
        TransferReceiver receiver(options, "peer");
        const auto ack = receiver.receive(*offer.value);
        ASSERT_TRUE(ack);
        ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*ack)), TransferError::None);
        for (int block = 0; block < 5; ++block) {
            const auto message = sender.next();
            ASSERT_TRUE(message);
            const auto reply = receiver.receive(*message);
            if (reply) {
                ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*reply)),
                          TransferError::None);
            }
        }
        EXPECT_EQ(receiver.progress().completedBytes, 5 * 65536U);
    }
    TransferSender sender;
    const auto offer = sender.prepare(request);
    ASSERT_TRUE(offer);
    TransferReceiver receiver(options, "peer");
    const auto ack = receiver.receive(*offer.value);
    ASSERT_TRUE(ack);
    EXPECT_EQ(std::get<protocol::FileAck>(*ack).nextSeq, 5);
    ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*ack)), TransferError::None);
    EXPECT_EQ(sender.progress().resumedBytes, 5 * 65536U);
    finish(sender, receiver);
    const auto output = receiver.progress().path;
    TransferSender duplicate;
    const auto repeated = duplicate.prepare(request);
    ASSERT_TRUE(repeated);
    const auto finalAck = receiver.receive(*repeated.value);
    ASSERT_TRUE(finalAck);
    ASSERT_EQ(duplicate.acknowledge(std::get<protocol::FileAck>(*finalAck)), TransferError::None);
    EXPECT_TRUE(std::holds_alternative<protocol::FileEnd>(*duplicate.next()));
    EXPECT_EQ(receiver.progress().path, output);
    EXPECT_EQ(receiver.progress().resumedBytes, offer.value->totalBytes);
}
TEST(TransferTest, InvalidChecksumNeverPromotesPartialDataAndAllowsAFreshAttempt) {
    const test::TemporaryDirectory directory;
    TransferReceiver receiver({directory.path() / "received", {}}, "peer");
    const protocol::FileBegin bad{"job", "PROJECT_ZIP", 3, std::string(64, '0'), 3};
    ASSERT_TRUE(receiver.receive(bad));
    ASSERT_TRUE(receiver.receive(protocol::FileData{"job", 0, {'a', 'b', 'c'}}));
    const auto reply = receiver.receive(protocol::FileEnd{"job", bad.sha256});
    ASSERT_TRUE(reply);
    EXPECT_EQ(std::get<protocol::FileErr>(*reply).reason, "sha_mismatch");
    EXPECT_EQ(receiver.progress().error, TransferError::ChecksumMismatch);
    for (const auto& entry : std::filesystem::directory_iterator(directory.path() / "received"))
        EXPECT_NE(entry.path().extension(), ".zip");
    auto valid = bad;
    valid.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto restarted = receiver.receive(valid);
    ASSERT_TRUE(restarted);
    EXPECT_EQ(std::get<protocol::FileAck>(*restarted).nextSeq, 0);
}
TEST(TransferTest, QuotaReservationsApplyAcrossPeersAndSurviveRestart) {
    const test::TemporaryDirectory directory;
    const TransferOptions options{directory.path() / "received", 100};
    const protocol::FileBegin offer{"job", "PROJECT_ZIP", 80, std::string(64, '0'), 10};
    {
        TransferReceiver receiver(options, "one");
        const auto result = receiver.receive(offer);
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<protocol::FileAck>(*result));
    }
    TransferReceiver other(options, "two");
    const auto rejected = other.receive(offer);
    ASSERT_TRUE(rejected);
    EXPECT_EQ(std::get<protocol::FileErr>(*rejected).reason, "disk_full");
    TransferReceiver resumed(options, "one");
    const auto accepted = resumed.receive(offer);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(std::get<protocol::FileAck>(*accepted).nextSeq, 0);
}
TEST(TransferTest, InvalidBlocksConflictingOffersAndConcurrentWritersCannotCorruptAPartialFile) {
    const test::TemporaryDirectory directory;
    const TransferOptions options{directory.path() / "received", {}};
    const protocol::FileBegin offer{"job", "PROJECT_ZIP", 8, std::string(64, '0'), 4};
    TransferReceiver receiver(options, "peer");
    ASSERT_TRUE(receiver.receive(offer));
    TransferReceiver concurrent(options, "peer");
    const auto busy = concurrent.receive(offer);
    ASSERT_TRUE(busy);
    EXPECT_EQ(std::get<protocol::FileErr>(*busy).reason, "aborted");
    const auto outOfOrder = receiver.receive(protocol::FileData{"job", 1, {1, 2, 3, 4}});
    ASSERT_TRUE(outOfOrder);
    EXPECT_EQ(receiver.progress().error, TransferError::InvalidSequence);
    auto changed = offer;
    changed.totalBytes = 9;
    const auto conflict = receiver.receive(changed);
    ASSERT_TRUE(conflict);
    EXPECT_EQ(std::get<protocol::FileErr>(*conflict).reason, "aborted");
    TransferReceiver tooShort(options, "another");
    ASSERT_TRUE(tooShort.receive(offer));
    const auto shortBlock = tooShort.receive(protocol::FileData{"job", 0, {1}});
    ASSERT_TRUE(shortBlock);
    EXPECT_EQ(std::get<protocol::FileErr>(*shortBlock).reason, "aborted");
}
TEST(TransferTest, FileIdsCannotEscapeTheStorageDirectoryAndSenderWindowIsBounded) {
    const test::TemporaryDirectory directory;
    const auto file = directory.path() / "source.zip";
    generate(file, 4096 * 20);
    TransferSender sender;
    const auto offer = sender.prepare({file, "../../outside", "PROJECT_ZIP", 4096});
    ASSERT_TRUE(offer);
    const auto root = directory.path() / "received";
    TransferReceiver receiver({root, {}}, "peer");
    const auto ack = receiver.receive(*offer.value);
    ASSERT_TRUE(ack);
    EXPECT_EQ(receiver.progress().path.parent_path(), std::filesystem::weakly_canonical(root));
    EXPECT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*ack)), TransferError::None);
    for (std::uint32_t block = 0; block < windowBlocks; ++block)
        ASSERT_TRUE(sender.next());
    EXPECT_FALSE(sender.next());
    EXPECT_EQ(sender.acknowledge({offer.value->fileId, windowBlocks + 1}),
              TransferError::InvalidSequence);
    // Un ACK tardif après une préparation invalide ne doit pas exploiter chunkSize == 0.
    EXPECT_FALSE(sender.prepare({file, "job", "PROJECT_ZIP", 0}));
    EXPECT_EQ(sender.acknowledge({"job", 0}), TransferError::InvalidSequence);
}
TEST(TransferTest, UncheckpointedTailIsTruncatedAndAbortingFreesTheReservation) {
    const test::TemporaryDirectory directory;
    const TransferOptions options{directory.path() / "received", 100};
    const protocol::FileBegin offer{"job", "PROJECT_ZIP", 20, std::string(64, '0'), 4};
    {
        TransferReceiver receiver(options, "peer");
        ASSERT_TRUE(receiver.receive(offer));
        EXPECT_FALSE(receiver.receive(protocol::FileData{"job", 0, {1, 2, 3, 4}}));
    }
    std::filesystem::path metadata;
    for (const auto& entry : std::filesystem::directory_iterator(options.directory))
        if (entry.path().extension() == ".json")
            metadata = entry.path();
    ASSERT_FALSE(metadata.empty());
    auto saved = nlohmann::json::parse(test::readFile(metadata));
    saved["nextSeq"] = 0; // Arrêt brutal avant publication du dernier checkpoint.
    test::writeFile(metadata, saved.dump());
    TransferReceiver receiver(options, "peer");
    const auto ack = receiver.receive(offer);
    ASSERT_TRUE(ack);
    EXPECT_EQ(std::get<protocol::FileAck>(*ack).nextSeq, 0);
    EXPECT_EQ(std::filesystem::file_size(receiver.progress().path), 0);
    receiver.receive(protocol::FileErr{"job", "aborted"});
    EXPECT_FALSE(std::filesystem::exists(metadata));
    EXPECT_EQ(receiver.progress().state, TransferState::Failed);
    EXPECT_EQ(receiver.progress().error, TransferError::Aborted);
}
TEST(TransferTest, AValidatedFileSurvivesACrashBetweenRenameAndCompletionCheckpoint) {
    const test::TemporaryDirectory directory;
    const auto source = directory.path() / "source";
    test::writeFile(source, "abc");
    const TransferOptions options{directory.path() / "received", {}};
    TransferSender sender;
    auto offer = sender.prepare({source, "job"});
    ASSERT_TRUE(offer);
    std::filesystem::path result;
    {
        TransferReceiver receiver(options, "peer");
        auto incoming = *offer.value;
        for (auto& value : incoming.sha256)
            if (value >= 'a' && value <= 'f')
                value = char(value - 'a' + 'A');
        const auto ack = receiver.receive(incoming);
        ASSERT_TRUE(ack);
        ASSERT_EQ(sender.acknowledge(std::get<protocol::FileAck>(*ack)), TransferError::None);
        finish(sender, receiver);
        result = receiver.progress().path;
    }
    auto metadata = result;
    metadata.replace_extension(".json");
    auto saved = nlohmann::json::parse(test::readFile(metadata));
    saved["complete"] = false;
    test::writeFile(metadata, saved.dump());
    TransferReceiver recovered(options, "peer");
    const auto ack = recovered.receive(*offer.value);
    ASSERT_TRUE(ack);
    EXPECT_EQ(std::get<protocol::FileAck>(*ack).nextSeq, 1);
    EXPECT_EQ(recovered.progress().path, result);
    EXPECT_EQ(test::readFile(result), "abc");
}
#ifndef _WIN32
TEST(TransferTest, APreexistingSymlinkCannotOverwriteAnOutsideFile) {
    const test::TemporaryDirectory directory;
    const auto root = directory.path() / "received";
    std::filesystem::create_directory(root);
    const auto victim = directory.path() / "outside";
    test::writeFile(victim, "untouched");
    const auto key = storageKey(std::string("peer") + '\0' + "job");
    std::filesystem::create_symlink(victim, root / (key + ".part"));
    TransferReceiver receiver({root, {}}, "peer");
    const auto reply =
        receiver.receive(protocol::FileBegin{"job", "PROJECT_ZIP", 3, std::string(64, '0'), 3});
    ASSERT_TRUE(reply);
    EXPECT_EQ(std::get<protocol::FileErr>(*reply).reason, "aborted");
    EXPECT_EQ(test::readFile(victim), "untouched");
}
#endif
