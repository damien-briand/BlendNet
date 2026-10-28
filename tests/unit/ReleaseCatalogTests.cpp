#include "blender/ReleaseCatalog.h"

#include <gtest/gtest.h>

#include <map>

using namespace blendnet::blender;
namespace {
std::string index(std::initializer_list<std::string> names) {
    std::string html = "<html><pre>";
    for (const auto& name : names)
        html += "<a href=\"" + name + "\">" + name + "</a>\n";
    return html + "</pre></html>";
}
struct Repository {
    const std::string root{releaseRepository};
    std::map<std::string, std::string> pages{
        {root, index({"../", "Blender2.93/", "Blender4.2/", "Blender4.10/", "Blender5.0/",
                      "Blender5.0/", "Blender6.0beta/", "Blender04.2/",
                      "https://example.com/Blender7.0/", "../Blender8.0/"})},
        {root + "Blender4.2/",
         index({"blender-4.2.9-linux-x64.tar.xz", "blender-4.2.10-linux-x64.tar.xz",
                "blender-4.2.9-windows-x64.zip", "blender-4.2.10-windows-x64.zip",
                "blender-4.2.9.sha256", "blender-4.2.10.sha256",
                "blender-4.2.11-linux-x64.tar.xz", // no checksum
                "blender-4.2.12.sha256"})}, // no archive
        {root + "Blender4.10/",
         index({"blender-4.10.0-linux-x64.tar.xz", "blender-4.10.0.sha256"})},
        {root + "Blender5.0/",
         index({"blender-5.0.2-linux-x64.tar.xz", "blender-5.0.2.sha256",
                "blender-5.0.0-windows-x64.zip", "blender-5.0.0.sha256",
                "blender-5.0.3-windows-x64.msi", "blender-5.0.3.sha256",
                "blender-5.0.4-windows-arm64.zip", "blender-5.0.4.sha256",
                "blender-5.0.5-beta-linux-x64.tar.xz", "blender-5.0.5-beta.sha256",
                "blender-5.1.0-linux-x64.tar.xz", "blender-5.1.0.sha256",
                "https://example.com/blender-5.0.9-linux-x64.tar.xz",
                "blender-5.0.9.sha256"})}};
    int requests = 0;
    FetchListing fetch() {
        return [this](const std::string& url, const std::atomic_bool*) -> BlenderResult<std::string> {
            ++requests;
            const auto page = pages.find(url);
            if (page == pages.end())
                return {{}, BlenderError::DownloadFailed};
            return {page->second, BlenderError::None};
        };
    }
};
} // namespace
TEST(ReleaseCatalogTest, StableLinuxArchivesRequireChecksumsAndSortNumerically) {
    Repository repository;
    const auto result = availableReleases({System::Linux, Architecture::X64}, nullptr,
                                          repository.fetch());
    ASSERT_TRUE(result);
    EXPECT_EQ(*result.value, (std::vector<std::string>{"5.0.2", "4.10.0", "4.2.10", "4.2.9"}));
    EXPECT_EQ(repository.requests, 4);
}
TEST(ReleaseCatalogTest, WindowsRequiresZipAndMatchesTheMachineArchitecture) {
    Repository repository;
    const auto x64 = availableReleases({System::Windows, Architecture::X64}, nullptr,
                                       repository.fetch());
    ASSERT_TRUE(x64);
    EXPECT_EQ(*x64.value, (std::vector<std::string>{"5.0.0", "4.2.10", "4.2.9"}));
    const auto arm64 = availableReleases({System::Windows, Architecture::Arm64}, nullptr,
                                         repository.fetch());
    ASSERT_TRUE(arm64);
    EXPECT_EQ(*arm64.value, (std::vector<std::string>{"5.0.4"}));
}
TEST(ReleaseCatalogTest, FailedOrEmptyListingsDoNotProduceAPartialCatalogue) {
    Repository repository;
    repository.pages.erase(repository.root + "Blender4.2/");
    auto result = availableReleases({System::Linux, Architecture::X64}, nullptr, repository.fetch());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, BlenderError::DownloadFailed);
    repository.pages = {{repository.root, "<html>Maintenance</html>"}};
    EXPECT_FALSE(availableReleases({System::Linux, Architecture::X64}, nullptr, repository.fetch()));
    repository.pages[repository.root] = std::string(1024 * 1024 + 1, 'x');
    EXPECT_FALSE(availableReleases({System::Linux, Architecture::X64}, nullptr, repository.fetch()));
}
TEST(ReleaseCatalogTest, CancellationNeverReturnsASelection) {
    Repository repository;
    std::atomic_bool cancelled{true};
    auto result = availableReleases({System::Linux, Architecture::X64}, &cancelled,
                                    repository.fetch());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, BlenderError::Cancelled);
    EXPECT_EQ(repository.requests, 0);
    cancelled = false;
    const auto fetch = repository.fetch();
    result = availableReleases({System::Linux, Architecture::X64}, &cancelled,
                               [&](const auto& url, const auto* stop) {
                                   cancelled = true;
                                   return fetch(url, stop);
                               });
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, BlenderError::Cancelled);
}
TEST(ReleaseCatalogTest, ListingTransportRejectsOtherOriginsAndHonoursCancellation) {
    EXPECT_EQ(fetchOfficialListing("https://example.com/release/", nullptr).error,
              BlenderError::InvalidRequest);
    EXPECT_EQ(fetchOfficialListing(std::string(releaseRepository) + "../", nullptr).error,
              BlenderError::InvalidRequest);
    const std::atomic_bool cancelled{true};
    EXPECT_EQ(fetchOfficialListing(std::string(releaseRepository), &cancelled).error,
              BlenderError::Cancelled);
}
