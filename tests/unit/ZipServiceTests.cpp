#include "TemporaryDirectory.h"
#include "archive/ZipService.h"
#include "jobs/ResultAssembler.h"

#include <gtest/gtest.h>
#include <zip.h>

using namespace blendnet;
namespace {
void createZip(const std::filesystem::path& path,
               const std::vector<std::pair<std::string, std::string>>& entries,
               bool symlink = false) {
    auto* zip = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    ASSERT_NE(zip, nullptr);
    for (const auto& [name, text] : entries) {
        auto* source = zip_source_buffer(zip, text.data(), text.size(), 0);
        ASSERT_NE(source, nullptr);
        const auto index = zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8);
        ASSERT_GE(index, 0);
        if (symlink) {
            ASSERT_EQ(
                zip_file_set_external_attributes(zip, index, 0, ZIP_OPSYS_UNIX, 0120777u << 16), 0);
        }
    }
    ASSERT_EQ(zip_close(zip), 0);
}
} // namespace
TEST(ZipServiceTest, ManifestSelectsSceneAndPreservesRelativeDependencies) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    createZip(
        zip, {{"scenes/main.blend", "main"},
              {"linked/library.blend", "linked"},
              {"textures/image.bin", "texture"},
              {"blendnet-project.json", R"({"schemaVersion":1,"blendPath":"scenes/main.blend"})"}});
    const auto target = directory.path() / "unpacked";
    const auto scene = archive::ZipService::extractProject(zip, target);
    ASSERT_TRUE(scene);
    EXPECT_EQ(*scene, target / "scenes/main.blend");
    EXPECT_EQ(test::readFile(target / "textures/image.bin"), "texture");
    EXPECT_EQ(test::readFile(target / "linked/library.blend"), "linked");
}
TEST(ZipServiceTest, UniqueSceneFallbackAndAmbiguousProjectRejection) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    createZip(zip, {{"scene.blend", "main"}});
    ASSERT_TRUE(archive::ZipService::extractProject(zip, directory.path() / "single"));
    createZip(zip, {{"scene.blend", "main"}, {"other.blend", "other"}});
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "ambiguous"));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "ambiguous"));
}
TEST(ZipServiceTest, RejectsTraversalPortableAliasesAndFileDirectoryConflicts) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    for (const auto* name : {"../escape", "/absolute", "C:/drive", "a/../escape", "a\\escape",
                             "a//escape", "CON.blend", "aux.txt", "a./file", "space /file"}) {
        createZip(zip, {{"scene.blend", "main"}, {name, "malicious"}});
        EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "rejected"))
            << name;
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "rejected"));
    }
    createZip(zip, {{"scene.blend", "main"}, {"SCENE.blend", "alias"}});
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "rejected"));
    createZip(zip, {{"scene.blend", "main"}, {"assets", "file"}, {"assets/data", "conflict"}});
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "rejected"));
}
TEST(ZipServiceTest, RejectsSymlinksAndExistingTargetsWithoutOverwriting) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    createZip(zip, {{"scene.blend", "../outside"}}, true);
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "rejected"));
    createZip(zip, {{"scene.blend", "main"}});
    const auto target = directory.path() / "existing";
    std::filesystem::create_directory(target);
    test::writeFile(target / "precious", "preserve");
    EXPECT_FALSE(archive::ZipService::extractProject(zip, target));
    EXPECT_EQ(test::readFile(target / "precious"), "preserve");
#ifndef _WIN32
    std::filesystem::create_directory_symlink(target, directory.path() / "link");
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "link/child"));
    EXPECT_FALSE(std::filesystem::exists(target / "child"));
#endif
}
TEST(ZipServiceTest, RejectsZipBombBudgetsAndCancellation) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    createZip(zip, {{"scene.blend", "scene"}, {"large.bin", std::string(100000, 'x')}});
    archive::ZipLimits limits;
    limits.expandedBytes = 1024;
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "size", limits));
    limits = {};
    limits.entries = 1;
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "count", limits));
    limits = {};
    limits.fileBytes = 1024;
    EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "file", limits));
    const std::atomic_bool cancelled{true};
    EXPECT_FALSE(
        archive::ZipService::extractProject(zip, directory.path() / "cancel", {}, &cancelled));
}
TEST(ZipServiceTest, InvalidManifestCannotSelectOutsideOrMissingScene) {
    test::TemporaryDirectory directory;
    const auto zip = directory.path() / "project.zip";
    for (const auto* manifest : {R"({"schemaVersion":1,"blendPath":"../scene.blend"})",
                                 R"({"schemaVersion":1,"blendPath":"missing.blend"})",
                                 R"({"schemaVersion":2,"blendPath":"scene.blend"})", "{"}) {
        createZip(zip, {{"scene.blend", "main"}, {"blendnet-project.json", manifest}});
        EXPECT_FALSE(archive::ZipService::extractProject(zip, directory.path() / "rejected"));
        EXPECT_FALSE(std::filesystem::exists(directory.path() / "rejected"));
    }
}
TEST(ZipServiceTest, ResultContainsOnlyAssignedInspectedFramesAndPartialErrorReport) {
    test::TemporaryDirectory directory;
    const protocol::TaskAssign task{"job", "chunk", 1, 3, 1, "frame_####", "PNG"};
    const auto fixture = directory.path() / "fixture.zip";
    ASSERT_TRUE(jobs::ResultAssembler::simulatedResult(task, fixture));
    const auto frames = directory.path() / "frames";
    ASSERT_EQ(jobs::ResultAssembler::validate(task, fixture, frames).size(), 3u);
    test::writeFile(frames / "other.txt", "must not be sent");
    const auto result = directory.path() / "result.zip";
    ASSERT_TRUE(archive::ZipService::packResult(task, frames, result));
    EXPECT_EQ(jobs::ResultAssembler::validate(task, result, directory.path() / "verified").size(),
              3u);
    std::filesystem::remove(frames / "frame_0002.png");
    const auto partial = directory.path() / "partial.zip";
    EXPECT_FALSE(archive::ZipService::packResult(task, frames, partial));
    EXPECT_FALSE(std::filesystem::exists(partial));
    ASSERT_TRUE(archive::ZipService::packResult(task, frames, partial, "render process failed"));
    auto* zip = zip_open(partial.string().c_str(), ZIP_RDONLY, nullptr);
    ASSERT_NE(zip, nullptr);
    EXPECT_EQ(zip_get_num_entries(zip, 0), 3);
    EXPECT_GE(zip_name_locate(zip, "blendnet-errors.json", 0), 0);
    EXPECT_LT(zip_name_locate(zip, "frame_0002.png", 0), 0);
    EXPECT_LT(zip_name_locate(zip, "other.txt", 0), 0);
    zip_discard(zip);
    // An incomplete chunk cannot count as a successful job on the source.
    EXPECT_TRUE(
        jobs::ResultAssembler::validate(task, partial, directory.path() / "incomplete").empty());
}
