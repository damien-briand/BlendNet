#include "TemporaryDirectory.h"
#include "network/NetworkTypes.h"
#include "network/Security.h"

#include <gtest/gtest.h>

#include <thread>

using namespace blendnet;
using namespace blendnet::network;
using namespace std::chrono_literals;

TEST(NetworkTest, ReconnectBackoffStartsAtOneSecondCapsAtFiveMinutesAndResets) {
    ReconnectPolicy policy;
    for (const auto seconds : {1, 2, 4, 8, 16, 32, 64, 128, 256, 300, 300})
        EXPECT_EQ(policy.nextDelay(), std::chrono::seconds(seconds));
    policy.reset();
    EXPECT_EQ(policy.nextDelay(), 1s);
    ReconnectPolicy odd(1ms, 5ms);
    for (const auto count : {1, 2, 4, 5, 5})
        EXPECT_EQ(odd.nextDelay(), std::chrono::milliseconds(count));
}
TEST(NetworkTest, IdentityAndTokenArePersistentAndPrivate) {
    const test::TemporaryDirectory directory;
    ServerIdentity first, second;
    ASSERT_EQ(ensureServerIdentity(directory.path(), first), NetworkError::None);
    ASSERT_EQ(ensureServerIdentity(directory.path(), second), NetworkError::None);
    EXPECT_EQ(first.token, second.token);
    EXPECT_EQ(first.token.size(), 64);
    const auto different = randomToken();
    ASSERT_TRUE(different);
    EXPECT_NE(first.token, *different);
    EXPECT_TRUE(sameToken(first.token, second.token));
    EXPECT_FALSE(sameToken(first.token, *different));
    EXPECT_FALSE(sameToken(first.token, first.token + "x"));
#ifndef _WIN32
    for (const auto& path : {first.privateKeyFile, first.tokenFile}) {
        const auto permissions = std::filesystem::status(path).permissions();
        EXPECT_EQ(permissions &
                      (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
                  std::filesystem::perms::none);
    }
#endif
}
TEST(NetworkTest, PersistingAPinPreservesSettingsAndCannotSilentlyReplaceIt) {
    const test::TemporaryDirectory directory;
    const auto path = directory.path() / "settings.json";
    AppSettings settings;
    settings.language = "en";
    settings.knownSources.push_back({"source", 47050, "test-token", {}});
    ASSERT_EQ(settings.save(path), SettingsError::None);
    ASSERT_EQ(persistPin(path, settings.knownSources[0], std::string(64, 'a')), NetworkError::None);
    ASSERT_EQ(AppSettings::load(path, settings), SettingsError::None);
    EXPECT_EQ(settings.language, "en");
    ASSERT_EQ(settings.knownSources.size(), 1);
    EXPECT_EQ(settings.knownSources[0].certificateSha256, std::string(64, 'a'));
    EXPECT_EQ(persistPin(path, settings.knownSources[0], std::string(64, 'b')),
              NetworkError::CertificateMismatch);
    EXPECT_TRUE(sameFingerprint(std::string(64, 'A'), std::string(64, 'a')));
}
TEST(NetworkTest, EventQueueCanBeDrainedWhileNetworkThreadsPublish) {
    EventQueue<int> queue;
    {
        std::vector<std::jthread> threads;
        for (int worker = 0; worker < 4; ++worker)
            threads.emplace_back([&queue, worker] {
                for (int item = 0; item < 100; ++item)
                    queue.push(worker * 100 + item);
            });
    }
    EXPECT_EQ(queue.drain().size(), 400);
    EXPECT_TRUE(queue.drain().empty());
    for (int item = 0; item < 2000; ++item)
        queue.push(item);
    EXPECT_EQ(queue.drain().size(), 1024);
}
