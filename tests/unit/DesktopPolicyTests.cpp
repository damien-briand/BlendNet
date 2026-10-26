#include "I18n.h"
#include "TemporaryDirectory.h"
#include "application/DesktopController.h"
#include <gtest/gtest.h>
#include <set>
using namespace blendnet;
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
