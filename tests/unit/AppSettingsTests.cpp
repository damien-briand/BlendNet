#include "TemporaryDirectory.h"
#include "config/AppSettings.h"

#include <gtest/gtest.h>

using namespace blendnet;
using namespace blendnet::test;

TEST(AppSettingsTest, MissingFilePreservesState) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "settings.json";
    AppSettings settings;
    settings.language = "en";
    const auto before = settings;

    EXPECT_TRUE(AppSettings::load(path, settings) == SettingsError::NotFound);
    EXPECT_TRUE(settings == before);
    EXPECT_FALSE(std::filesystem::exists(path));

    EXPECT_EQ(AppSettings::load(directory.path() / "missing" / "settings.json", settings),
              SettingsError::NotFound);
    EXPECT_TRUE(settings == before);
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "missing"));
}

TEST(AppSettingsTest, RoundTripAndAtomicReplacement) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "nested" / "settings.json";
    AppSettings expected;
    expected.language = "en";
    expected.computeBackend = "OPTIX";
    expected.logLevel = LogLevel::Debug;
    expected.benchmarkScore = 456;
    expected.keepCompletedJobs = true;
    expected.workDirectory = std::filesystem::path(u8"projets/Scènes");
    expected.blenderCacheDirectory = "cache/blender";
    expected.knownSources = {
        {"render.example.org", 47050, "secret-test-token", std::string(64, 'a')},
        {"::1", 65535, "other-test-token", std::nullopt}};

    ASSERT_TRUE(expected.save(path) == SettingsError::None);
    AppSettings actual;
    ASSERT_TRUE(AppSettings::load(path, actual) == SettingsError::None);
    EXPECT_TRUE(actual == expected);

    expected.language = "fr";
    expected.benchmarkScore.reset();
    expected.knownSources.clear();
    ASSERT_TRUE(expected.save(path) == SettingsError::None);
    ASSERT_TRUE(AppSettings::load(path, actual) == SettingsError::None);
    EXPECT_TRUE(actual == expected);
    EXPECT_TRUE(std::distance(std::filesystem::directory_iterator(path.parent_path()),
                              std::filesystem::directory_iterator{}) == 1);
#ifndef _WIN32
    const auto permissions = std::filesystem::status(path).permissions();
    EXPECT_TRUE((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    EXPECT_TRUE((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
}

TEST(AppSettingsTest, PartialSettingsUseDefaults) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "settings.json";
    writeFile(path, R"({"language":"en"})");
    AppSettings settings;
    settings.knownSources.push_back({"old.example", 47050, "old-token", std::nullopt});

    ASSERT_TRUE(AppSettings::load(path, settings) == SettingsError::None);
    AppSettings expected;
    expected.language = "en";
    EXPECT_TRUE(settings == expected);
}

TEST(AppSettingsTest, MalformedJsonAndUnsupportedVersion) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "settings.json";
    AppSettings settings;
    settings.language = "en";
    const auto before = settings;

    {
        writeFile(path, R"({"knownSources": [)");
        EXPECT_TRUE(AppSettings::load(path, settings) == SettingsError::InvalidJson);
    }
    {
        writeFile(path, R"({"schemaVersion":2})");
        EXPECT_TRUE(AppSettings::load(path, settings) == SettingsError::UnsupportedVersion);
    }
    EXPECT_TRUE(settings == before);
}

TEST(AppSettingsTest, InvalidTypesAndRangesPreserveState) {
    const char* cases[] = {
        "[]",
        "null",
        R"({"language":"de"})",
        R"({"language":7})",
        R"({"computeBackend":"unknown"})",
        R"({"logLevel":"trace"})",
        R"({"workDirectory":""})",
        R"({"workDirectory":"\u0000"})",
        R"({"blenderCacheDirectory":false})",
        R"({"keepCompletedJobs":"yes"})",
        R"({"keepCompletedJobs":1})",
        R"({"benchmarkScore":-1})",
        R"({"benchmarkScore":1.5})",
        R"({"benchmarkScore":18446744073709551615})",
        R"({"schemaVersion":true})",
        R"({"schemaVersion":4294967297})",
        R"({"knownSources":{}})",
        R"({"knownSources":[{"host":"server","port":0,"token":"test"}]})",
        R"({"knownSources":[{"host":"server","port":65536,"token":"test"}]})",
        R"({"knownSources":[{"host":"server","port":4294967297,"token":"test"}]})",
        R"({"knownSources":[{"host":"server","port":47050.0,"token":"test"}]})",
        R"({"knownSources":[{"host":"server","port":47050,"token":""}]})",
        R"({"knownSources":[{"host":"bad host","port":47050,"token":"test"}]})",
        R"({"knownSources":[{"host":"server","port":47050,"token":"test","certificateSha256":"bad"}]})",
        R"({"knownSources":[{"host":"server"}]})"};
    for (const auto* content : cases) {
        SCOPED_TRACE(content);
        const TemporaryDirectory directory;
        const auto path = directory.path() / "settings.json";
        writeFile(path, content);
        AppSettings settings;
        settings.language = "en";
        const auto before = settings;

        const auto result = AppSettings::load(path, settings);
        EXPECT_TRUE(
            (result == SettingsError::InvalidValue || result == SettingsError::UnsupportedVersion));
        EXPECT_TRUE(settings == before);
        EXPECT_TRUE(readFile(path) == content);
    }
}

TEST(AppSettingsTest, InvalidSavePreservesExistingFile) {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "settings.json";
    AppSettings settings;
    ASSERT_TRUE(settings.save(path) == SettingsError::None);
    const auto before = readFile(path);
    settings.computeBackend = "invalid";

    EXPECT_TRUE(settings.save(path) == SettingsError::InvalidValue);
    EXPECT_TRUE(readFile(path) == before);
}

TEST(AppSettingsTest, FileSystemErrors) {
    const TemporaryDirectory directory;
    const auto obstruction = directory.path() / "file";
    writeFile(obstruction, "not a directory");
    AppSettings settings;
    settings.language = "en";
    const auto before = settings;

    EXPECT_EQ(settings.save(obstruction / "settings.json"), SettingsError::WriteFailed);
    EXPECT_EQ(settings.save(directory.path()), SettingsError::WriteFailed);
    EXPECT_EQ(AppSettings::load(obstruction / "settings.json", settings),
              SettingsError::ReadFailed);
    EXPECT_EQ(AppSettings::load(directory.path(), settings), SettingsError::ReadFailed);
    EXPECT_TRUE(settings == before);
    EXPECT_TRUE(readFile(obstruction) == "not a directory");
}
