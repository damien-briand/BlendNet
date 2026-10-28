#include "TemporaryDirectory.h"
#include "blender/BlenderInstaller.h"
#include "config/Utf8Path.h"

#include <gtest/gtest.h>
#include <zip.h>

using namespace blendnet;
using namespace blendnet::blender;
namespace {
void createZip(const std::filesystem::path& path, const std::string& name, bool link = false) {
    auto* zip = zip_open(utf8(path).c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    ASSERT_NE(zip, nullptr);
    static constexpr char content[] = "official layout fixture";
    auto* source = zip_source_buffer(zip, content, sizeof(content) - 1, 0);
    ASSERT_NE(source, nullptr);
    const auto entry = zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8);
    ASSERT_GE(entry, 0);
    ASSERT_EQ(zip_file_set_external_attributes(zip, entry, 0,
                                              link ? ZIP_OPSYS_UNIX : ZIP_OPSYS_DOS,
                                              link ? (0120777u << 16) : 0x20), 0);
    ASSERT_EQ(zip_close(zip), 0);
}
Release windowsRelease() {
    return *officialRelease("4.2.1", {System::Windows, Architecture::X64}).value;
}
} // namespace
TEST(BlenderArchiveTest, OfficialLayoutExtractsPathsLongerThanMaxPath) {
    test::TemporaryDirectory directory;
    const auto release = windowsRelease();
    // One of the actual long names in the official Windows 4.2.1 ZIP.
    const std::string name = release.topDirectory +
        "/4.2/scripts/addons_core/io_scene_gltf2/blender/exp/animation/sampled/object/"
        "gltf2_blender_gather_object_channel_target.py";
    const auto zip = directory.path() / "blender.zip";
    createZip(zip, name);
    const auto destination = directory.path() / std::string(90, 'c') / "content";
    const auto target = destination / pathFromUtf8(name);
    ASSERT_GT(target.native().size(), 260u);
    std::filesystem::create_directories(extendedFilePath(destination));
    std::string diagnostic;
    ASSERT_EQ(blender::detail::extractBlenderZip(release, zip, destination, nullptr, diagnostic),
              BlenderError::None) << diagnostic;
    EXPECT_EQ(test::readFile(extendedFilePath(target)), "official layout fixture");
    // Remove long names through the same namespace, including on machines whose policy is off.
    std::filesystem::remove_all(extendedFilePath(destination.parent_path()));
}
TEST(BlenderArchiveTest, FileSystemFailureIsReportedWithTheOffendingPath) {
    test::TemporaryDirectory directory;
    const auto release = windowsRelease();
    const auto zip = directory.path() / "blender.zip";
    createZip(zip, release.topDirectory + "/blender.exe");
    const auto destination = directory.path() / "content";
    std::filesystem::create_directory(destination);
    test::writeFile(destination / release.topDirectory, "preserve");
    std::string diagnostic;
    EXPECT_EQ(blender::detail::extractBlenderZip(release, zip, destination, nullptr, diagnostic),
              BlenderError::IoError);
    EXPECT_NE(diagnostic.find(release.topDirectory), std::string::npos);
    EXPECT_EQ(test::readFile(destination / release.topDirectory), "preserve");
}
TEST(BlenderArchiveTest, TraversalAndLinksRemainRejectedBeforeExtraction) {
    test::TemporaryDirectory directory;
    const auto release = windowsRelease();
    const auto zip = directory.path() / "blender.zip";
    const auto destination = directory.path() / "content";
    std::filesystem::create_directory(destination);
    std::string diagnostic;
    for (const auto& name : {release.topDirectory + "/../escape", std::string("C:/escape"),
                             std::string("other/blender.exe")}) {
        createZip(zip, name);
        EXPECT_EQ(blender::detail::extractBlenderZip(release, zip, destination, nullptr, diagnostic),
                  BlenderError::ArchiveRejected);
        EXPECT_NE(diagnostic.find(name), std::string::npos);
    }
    createZip(zip, release.topDirectory + "/link", true);
    EXPECT_EQ(blender::detail::extractBlenderZip(release, zip, destination, nullptr, diagnostic),
              BlenderError::ArchiveRejected);
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "escape"));
}
TEST(BlenderArchiveTest, InvalidZipHasTheLibraryErrorAndArchivePath) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "invalid.zip";
    test::writeFile(zip, "not a ZIP");
    std::string diagnostic;
    EXPECT_EQ(blender::detail::extractBlenderZip(windowsRelease(), zip, directory.path(), nullptr,
                                               diagnostic), BlenderError::ArchiveRejected);
    EXPECT_NE(diagnostic.find("invalid.zip"), std::string::npos);
    EXPECT_GT(diagnostic.size(), std::string("Cannot open ZIP ").size() + utf8(zip).size());
}
#ifdef _WIN32
TEST(BlenderArchiveTest, ExtendedNamesNormalizeDriveAndUncPathsAndAreIdempotent) {
    EXPECT_EQ(extendedFilePath(std::filesystem::path(L"C:/cache/sub/../Blender")).native(),
              L"\\\\?\\C:\\cache\\Blender");
    EXPECT_EQ(extendedFilePath(std::filesystem::path(L"\\\\server\\share\\cache")).native(),
              L"\\\\?\\UNC\\server\\share\\cache");
    const auto path = extendedFilePath(std::filesystem::path(L"C:/cache"));
    EXPECT_EQ(extendedFilePath(path), path);
}
#endif
