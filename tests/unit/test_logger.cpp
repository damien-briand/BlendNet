#include <gtest/gtest.h>

#include "log/Logger.h"

using namespace blendnet;

namespace {
struct LoggerFixture : ::testing::Test {
    QStringList lines;
    void SetUp() override {
        auto& l = Logger::instance();
        l.clearSecrets();
        l.setLevel(LogLevel::Info);
        l.setSink([this](const QString& s) { lines << s; });
    }
    void TearDown() override {
        Logger::instance().setSink(nullptr);
        Logger::instance().clearSecrets();
    }
};
} // namespace

TEST_F(LoggerFixture, FiltersBelowLevel) {
    BN_DEBUG("t", "invisible");
    BN_INFO("t", "visible");
    ASSERT_EQ(lines.size(), 1);
    EXPECT_TRUE(lines[0].contains("visible"));
    EXPECT_TRUE(lines[0].contains("[INFO]"));
}

TEST_F(LoggerFixture, RedactsRegisteredSecrets) {
    Logger::instance().addSecret("s3cr3t-token");
    BN_INFO("net", "HELLO token=s3cr3t-token user=bob");
    ASSERT_EQ(lines.size(), 1);
    EXPECT_FALSE(lines[0].contains("s3cr3t-token"));
    EXPECT_TRUE(lines[0].contains("token=***"));
}

TEST_F(LoggerFixture, LevelFromString) {
    EXPECT_EQ(Logger::levelFromString("debug"), LogLevel::Debug);
    EXPECT_EQ(Logger::levelFromString("nope", LogLevel::Warn), LogLevel::Warn);
}
