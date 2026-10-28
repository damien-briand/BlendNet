#include "I18n.h"
#include "TemporaryDirectory.h"
#include "application/DesktopController.h"
#include <gtest/gtest.h>
#include <sstream>
#include <set>
#include <thread>
using namespace blendnet;
namespace {
class DesktopCatalogueTest : public ::testing::Test {
  protected:
    test::TemporaryDirectory directory;
    std::ostringstream log;
    Logger logger{log};
    AppSettings settings;
    const std::filesystem::path config = directory.path() / "settings.json";
    std::atomic_bool offline{false}, frozen{false};
    std::unique_ptr<application::DesktopController> desktop;
    void start() {
        settings.workDirectory = directory.path() / "jobs";
        settings.blenderCacheDirectory = directory.path() / "cache";
        ASSERT_EQ(settings.save(config), SettingsError::None);
        desktop = std::make_unique<application::DesktopController>(
            settings, config, logger,
            [this](const std::string& url,
                   const std::atomic_bool* cancelled) -> blender::BlenderResult<std::string> {
                while (frozen && !cancelled->load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (offline || cancelled->load())
                    return {{}, blender::BlenderError::DownloadFailed};
                if (url == blender::releaseRepository)
                    return {"<a href=\"Blender5.0/\">Blender5.0/</a>", blender::BlenderError::None};
                const auto platform = blender::Platform::current();
                if (!platform)
                    return {{}, blender::BlenderError::UnsupportedPlatform};
                std::string body;
                for (const auto* version : {"5.0.9", "5.0.10"}) {
                    const auto release = blender::officialRelease(version, *platform);
                    body += "<a href=\"" + release.value->archiveName + "\">archive</a>";
                    body += "<a href=\"blender-" + std::string(version) + ".sha256\">checksum</a>";
                }
                return {body, blender::BlenderError::None};
            });
    }
    template <typename Predicate> bool await(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate(desktop->snapshot()))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate(desktop->snapshot());
    }
};
} // namespace
TEST_F(DesktopCatalogueTest, EmptyConfigurationSelectsAndPersistsHighestVersion) {
    settings.language = "en";
    settings.benchmarkScore = 123;
    start();
    ASSERT_TRUE(await([](const auto& state) {
        return !state.busy && state.settings.blenderVersion == "5.0.10";
    }));
    EXPECT_EQ(desktop->snapshot().blenderVersions,
              (std::vector<std::string>{"5.0.10", "5.0.9"}));
    AppSettings saved;
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    EXPECT_EQ(saved.blenderVersion, "5.0.10");
    EXPECT_EQ(saved.language, "en");
    EXPECT_FALSE(saved.benchmarkScore);
    EXPECT_FALSE(std::filesystem::exists(settings.blenderCacheDirectory));
}
TEST_F(DesktopCatalogueTest, ConfiguredVersionAndCatalogueSurviveRefreshFailure) {
    settings.blenderVersion = "4.2.1";
    settings.benchmarkScore = 321;
    start();
    ASSERT_TRUE(await([](const auto& state) { return state.blenderVersions.size() == 2; }));
    offline = true;
    desktop->refreshBlenderVersions();
    ASSERT_TRUE(await([](const auto& state) { return state.blenderVersionsFailed; }));
    const auto snapshot = desktop->snapshot();
    EXPECT_EQ(snapshot.blenderVersions.size(), 2u);
    EXPECT_EQ(snapshot.settings.blenderVersion, "4.2.1");
    EXPECT_EQ(snapshot.settings.benchmarkScore, 321);
    AppSettings saved;
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    EXPECT_EQ(saved, settings);
}
TEST_F(DesktopCatalogueTest, EmptyOfflineConfigurationCanRetryWithoutInventingAVersion) {
    offline = true;
    start();
    ASSERT_TRUE(await([](const auto& state) { return state.blenderVersionsFailed; }));
    EXPECT_TRUE(desktop->snapshot().settings.blenderVersion.empty());
    offline = false;
    desktop->refreshBlenderVersions();
    ASSERT_TRUE(await([](const auto& state) {
        return !state.busy && state.settings.blenderVersion == "5.0.10";
    }));
}
TEST_F(DesktopCatalogueTest, UserSelectionWhileCatalogueLoadsIsPreserved) {
    frozen = true;
    start();
    ASSERT_TRUE(await([](const auto& state) {
        return state.blenderVersionsLoading && !state.busy;
    }));
    auto selected = desktop->snapshot().settings;
    selected.blenderVersion = "4.2.1";
    desktop->saveSettings(selected);
    ASSERT_TRUE(await([](const auto& state) {
        return !state.busy && state.settings.blenderVersion == "4.2.1";
    }));
    frozen = false;
    ASSERT_TRUE(await([](const auto& state) {
        return !state.blenderVersionsLoading && state.blenderVersions.size() == 2;
    }));
    EXPECT_EQ(desktop->snapshot().settings.blenderVersion, "4.2.1");
    AppSettings saved;
    ASSERT_EQ(AppSettings::load(config, saved), SettingsError::None);
    EXPECT_EQ(saved.blenderVersion, "4.2.1");
}
TEST_F(DesktopCatalogueTest, VerboseProbeFailureIncludesDetailsInTheFileAndExportedJournal) {
    settings.blenderVersion = "4.2.1";
    settings.blenderExecutable = directory.path() / "missing-blender.exe";
    settings.logLevel = LogLevel::Debug;
    start();
    ASSERT_TRUE(await([](const auto& state) { return !state.busy && state.status == "scan.done"; }));
    desktop->probe();
    ASSERT_TRUE(await([](const auto& state) { return !state.busy && state.status == "operation.failed"; }));
    EXPECT_NE(desktop->snapshot().detail.find("missing-blender.exe"), std::string::npos);
    EXPECT_NE(log.str().find("[debug]"), std::string::npos);
    EXPECT_NE(log.str().find("missing-blender.exe"), std::string::npos);
    const auto exported = directory.path() / "diagnostics.txt";
    desktop->exportLogs(exported);
    ASSERT_TRUE(await([](const auto& state) { return !state.busy && state.status == "export.done"; }));
    const auto text = test::readFile(exported);
    EXPECT_NE(text.find("Checking selected executable"), std::string::npos);
    EXPECT_NE(text.find("missing-blender.exe"), std::string::npos);
}
TEST(DesktopPolicyTest, OvernightScheduleAndLocalPriorityAreAppliedBeforeRecruitment) {
    AppSettings settings;
    settings.scheduleEnabled = true;
    settings.availableFromHour = 22;
    settings.availableUntilHour = 7;
    EXPECT_TRUE(application::scheduledAvailable(settings, 23));
    EXPECT_TRUE(application::scheduledAvailable(settings, 3));
    EXPECT_FALSE(application::scheduledAvailable(settings, 7));
    EXPECT_FALSE(application::scheduledAvailable(settings, 12));
    settings.excludedEngines = {"EEVEE"};
    settings.maxFramesPerJob = 20;
    settings.quotaGB = 2;
    settings.stopOnActivity = true;
    const auto night = application::clientPreferences(settings, 23, false);
    EXPECT_EQ(night.availability, "AVAILABLE");
    EXPECT_EQ(night.excludedEngines, settings.excludedEngines);
    EXPECT_EQ(night.maxFramesPerJob, 20);
    EXPECT_EQ(night.quotaGB, 2);
    EXPECT_EQ(application::clientPreferences(settings, 23, true).availability, "DND");
    EXPECT_EQ(application::clientPreferences(settings, 12, false).availability, "DND");
    settings.availableFromHour = settings.availableUntilHour;
    EXPECT_TRUE(application::scheduledAvailable(settings, 12));
}
TEST(DesktopPolicyTest, FrenchAndEnglishCataloguesAreCompleteAndHaveStableUniqueKeys) {
    std::set<std::string_view> keys;
    for (const auto& entry : ui::catalogue()) {
        EXPECT_TRUE(keys.insert(entry.key).second) << entry.key;
        EXPECT_FALSE(entry.fr.empty());
        EXPECT_FALSE(entry.en.empty());
        EXPECT_EQ(ui::translate(entry.key, "fr"), entry.fr);
        EXPECT_EQ(ui::translate(entry.key, "en"), entry.en);
    }
    EXPECT_EQ(ui::translate("Settings", "fr"), "Paramètres");
    for (int i = 0; i <= int(blender::BlenderError::MissingFrames); ++i) {
        const auto key = blender::toString(blender::BlenderError(i));
        EXPECT_NE(ui::translate(key, "fr"), key) << key;
    }
    for (int i = 0; i <= int(network::NetworkError::Stopped); ++i) {
        const auto key = network::toString(network::NetworkError(i));
        EXPECT_NE(ui::translate(key, "fr"), key) << key;
    }
}
TEST(DesktopPolicyTest, LibraryDeduplicatesFoldersAndRemovesHiddenProjectsWithoutFollowingLinks) {
    test::TemporaryDirectory directory;
    const auto nested = directory.path() / "scenes";
    std::filesystem::create_directory(nested);
    test::writeFile(nested / "animation.blend", "scene");
    test::writeFile(nested / "ignored.blend1", "backup");
    test::writeFile(directory.path() / "other.blend", "other");
#ifndef _WIN32
    std::filesystem::create_directory_symlink(directory.path(), nested / "loop");
#endif
    const auto list = projects::ProjectLibrary::scan({directory.path(), nested});
    ASSERT_EQ(list.size(), 2u);
    const auto filtered =
        projects::ProjectLibrary::scan({directory.path()}, {nested / "animation.blend"});
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].name, "other");
    const std::atomic_bool cancelled{true};
    EXPECT_TRUE(projects::ProjectLibrary::scan({directory.path()}, {}, &cancelled).empty());
}
