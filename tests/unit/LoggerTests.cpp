#include "TemporaryDirectory.h"
#include "log/Logger.h"

#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace blendnet;
using namespace blendnet::test;

TEST(LoggerTest, LevelsAndUtcTimestamp) {
    std::ostringstream output;
    Logger logger(output, LogLevel::Warning);
    EXPECT_TRUE(logger.log(LogLevel::Info, "filtered") == LogError::None);
    EXPECT_TRUE(output.str().empty());
    ASSERT_TRUE(logger.log(LogLevel::Warning, "visible") == LogError::None);
    EXPECT_TRUE(std::regex_match(
        output.str(), std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z \[warning\] visible\n)")));

    logger.setLevel(LogLevel::Debug);
    EXPECT_TRUE(logger.level() == LogLevel::Debug);
    EXPECT_TRUE(logger.log(LogLevel::Debug, "debug enabled") == LogError::None);
    EXPECT_TRUE(output.str().find("[debug] debug enabled") != std::string::npos);
    logger.setLevel(LogLevel::Off);
    const auto before = output.str();
    EXPECT_TRUE(logger.log(LogLevel::Error, "disabled") == LogError::None);
    EXPECT_TRUE(logger.log(LogLevel::Off, "disabled") == LogError::None);
    EXPECT_TRUE(output.str() == before);
}

TEST(LoggerTest, ConcurrentMessagesRemainComplete) {
    std::ostringstream output;
    Logger logger(output);
    {
        std::vector<std::jthread> threads;
        for (int worker = 0; worker < 4; ++worker) {
            threads.emplace_back([&logger, worker] {
                for (int index = 0; index < 100; ++index) {
                    logger.log(LogLevel::Info,
                               std::to_string(worker) + ":" + std::to_string(index));
                }
            });
        }
    }
    std::istringstream input(output.str());
    std::set<std::string> messages;
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        const auto marker = line.find(" [info] ");
        ASSERT_TRUE(marker != std::string::npos);
        messages.insert(line.substr(marker + 8));
        ++count;
    }
    EXPECT_TRUE(count == 400);
    EXPECT_TRUE(messages.size() == 400);
}

TEST(LoggerTest, AppendAndEscapeMultilineMessages) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "logs" / "blendnet.log";
    std::ostringstream output;
    {
        Logger logger(output);
        ASSERT_TRUE(logger.openFile(path) == LogError::None);
        ASSERT_TRUE(logger.log(LogLevel::Info, "first\nsecond\rthird") == LogError::None);
    }
    const auto first = readFile(path);
    EXPECT_TRUE(first.find("first\\nsecond\\rthird\n") != std::string::npos);
    EXPECT_TRUE(first.find('\n') == first.size() - 1);
    {
        Logger logger(output);
        ASSERT_TRUE(logger.openFile(path) == LogError::None);
        ASSERT_TRUE(logger.log(LogLevel::Error, "last") == LogError::None);
    }
    EXPECT_TRUE(readFile(path).starts_with(first));
    EXPECT_TRUE(readFile(path) == output.str());
}

TEST(LoggerTest, FileAndStreamErrors) {
    const TemporaryDirectory directory;
    std::ostringstream output;
    Logger logger(output);
    EXPECT_TRUE(logger.openFile(directory.path()) == LogError::OpenFailed);
    EXPECT_TRUE(logger.log(LogLevel::Info, "console still works") == LogError::None);
    output.setstate(std::ios::badbit);
    EXPECT_TRUE(logger.log(LogLevel::Error, "failed write") == LogError::WriteFailed);
}
