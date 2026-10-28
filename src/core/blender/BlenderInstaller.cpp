#include "blender/BlenderInstaller.h"
#include "blender/Process.h"
#include "config/Utf8Path.h"
#include "network/Security.h"
#include "transfer/TransferEngine.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <sstream>

namespace blendnet::blender {
namespace {
using Json = nlohmann::json;
bool stopped(const std::atomic_bool* flag) { return flag && flag->load(); }
bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
bool safeName(std::string_view name) {
    if (name.empty() || name.size() > 4096 || name.front() == '/' ||
        name.find_first_of("\\:\r\n") != name.npos || name.find('\0') != name.npos)
        return false;
    const auto path = pathFromUtf8(name);
    if (path.is_absolute())
        return false;
    for (const auto& part : path)
        if (part == "..")
            return false;
    return true;
}
void privateDirectory(const std::filesystem::path& path) {
    if (std::filesystem::is_symlink(path))
        throw std::runtime_error("Unsafe Blender cache");
    std::filesystem::create_directories(path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}
void removeOwned(const std::filesystem::path& path) {
    std::error_code ignored;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored)))
        return;
    std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, ignored);
    for (std::filesystem::recursive_directory_iterator it(path, ignored), end;
         it != end && !ignored; it.increment(ignored))
        if (!it->is_symlink())
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, ignored);
    std::filesystem::remove_all(path, ignored);
}
struct Temporary {
    std::filesystem::path path;
    bool keep = false;
    ~Temporary() {
        if (!keep)
            removeOwned(path);
    }
};
BlenderResult<std::string> treeHash(const std::filesystem::path& directory,
                                    const std::atomic_bool* cancelled) {
    try {
        if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory))
            return {{}, BlenderError::CacheCorrupt};
        const auto root = std::filesystem::canonical(directory);
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (stopped(cancelled))
                return {{}, BlenderError::Cancelled};
            if (paths.size() >= 100000)
                return {{}, BlenderError::ArchiveRejected};
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(),
                                                                     EVP_MD_CTX_free);
        if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
            return {{}, BlenderError::IoError};
        std::uint64_t total = 0;
        for (const auto& path : paths) {
            if (stopped(cancelled))
                return {{}, BlenderError::Cancelled};
            const auto status = std::filesystem::symlink_status(path);
            const auto name = utf8(path.lexically_relative(root));
            std::string item;
            if (std::filesystem::is_symlink(status)) {
                if (!inside(root, std::filesystem::canonical(path)))
                    return {{}, BlenderError::CacheCorrupt};
                item = "L" + name + "\n" + utf8(std::filesystem::read_symlink(path));
            } else if (std::filesystem::is_directory(status))
                item = "D" + name;
            else if (std::filesystem::is_regular_file(status)) {
                const auto bytes = std::filesystem::file_size(path);
                if (bytes > 8ULL * 1024 * 1024 * 1024 - total)
                    return {{}, BlenderError::ArchiveRejected};
                total += bytes;
                const auto digest = transfer::sha256File(path, cancelled);
                if (!digest)
                    return {{},
                            stopped(cancelled) ? BlenderError::Cancelled : BlenderError::IoError};
                item = "F" + name + "\n" + std::to_string(bytes) + "\n" + *digest.value;
            } else
                return {{}, BlenderError::ArchiveRejected};
            item.push_back('\0');
            if (EVP_DigestUpdate(hash.get(), item.data(), item.size()) != 1)
                return {{}, BlenderError::IoError};
        }
        std::array<unsigned char, 32> bytes{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(hash.get(), bytes.data(), &size) != 1 || size != 32)
            return {{}, BlenderError::IoError};
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for (auto b : bytes) {
            result += digits[b >> 4];
            result += digits[b & 15];
        }
        return {result, BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::CacheCorrupt};
    }
}
void seal(const std::filesystem::path& root) {
    // Le Python embarqué ne peut pas écrire de __pycache__ dans le logiciel vérifié.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (!entry.is_symlink())
            std::filesystem::permissions(entry.path(),
                                         std::filesystem::perms::owner_write |
                                             std::filesystem::perms::group_write |
                                             std::filesystem::perms::others_write,
                                         std::filesystem::perm_options::remove);
    std::filesystem::permissions(root,
                                 std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove);
}
BlenderError extractZip(const Release& release, const std::filesystem::path& archive,
                        const std::filesystem::path& destination,
                        const std::atomic_bool* cancelled) {
    std::unique_ptr<zip_t, decltype(&zip_discard)> zip(
        zip_open(utf8(archive).c_str(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr), zip_discard);
    if (!zip)
        return BlenderError::ArchiveRejected;
    const auto count = zip_get_num_entries(zip.get(), 0);
    if (count < 1 || count > 100000)
        return BlenderError::ArchiveRejected;
    std::uint64_t total = 0;
    std::set<std::string> names;
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat{};
        zip_uint8_t os = 0;
        zip_uint32_t attrs = 0;
        if (zip_stat_index(zip.get(), i, 0, &stat) < 0 || !stat.name || !safeName(stat.name) ||
            !std::string_view(stat.name).starts_with(release.topDirectory + "/") ||
            !names.insert(stat.name).second || stat.encryption_method != ZIP_EM_NONE ||
            zip_file_get_external_attributes(zip.get(), i, 0, &os, &attrs) < 0 ||
            (os == ZIP_OPSYS_UNIX && ((attrs >> 16) & 0170000) != 0 &&
             ((attrs >> 16) & 0170000) != 0100000 && ((attrs >> 16) & 0170000) != 0040000) ||
            stat.size > 8ULL * 1024 * 1024 * 1024 - total)
            return BlenderError::ArchiveRejected;
        total += stat.size;
    }
    if (std::filesystem::space(destination).available < total)
        return BlenderError::IoError;
    for (zip_int64_t i = 0; i < count; ++i) {
        if (stopped(cancelled))
            return BlenderError::Cancelled;
        zip_stat_t stat{};
        if (zip_stat_index(zip.get(), i, 0, &stat) < 0)
            return BlenderError::ArchiveRejected;
        const auto target = destination / pathFromUtf8(stat.name);
        if (std::string_view(stat.name).ends_with('/')) {
            std::filesystem::create_directories(target);
            continue;
        }
        std::filesystem::create_directories(target.parent_path());
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(zip.get(), i, 0),
                                                                zip_fclose);
        std::ofstream output(target, std::ios::binary);
        if (!file || !output)
            return BlenderError::ArchiveRejected;
        std::array<char, 65536> buffer{};
        std::uint64_t written = 0;
        while (true) {
            if (stopped(cancelled))
                return BlenderError::Cancelled;
            const auto bytes = zip_fread(file.get(), buffer.data(), buffer.size());
            if (bytes < 0)
                return BlenderError::ArchiveRejected;
            if (!bytes)
                break;
            written += bytes;
            if (written > stat.size)
                return BlenderError::ArchiveRejected;
            output.write(buffer.data(), bytes);
            if (!output)
                return BlenderError::IoError;
        }
        output.close();
        if (!output || written != stat.size)
            return BlenderError::ArchiveRejected;
    }
    return BlenderError::None;
}
BlenderError extract(const Release& release, const std::filesystem::path& archive,
                     const std::filesystem::path& destination, const std::atomic_bool* cancelled) {
    if (release.archiveName.ends_with(".zip"))
        return extractZip(release, archive, destination, cancelled);
    if (release.archiveName.ends_with(".tar.xz")) {
        // L'archive a déjà été authentifiée par le SHA-256 publié sur blender.org.
        const auto listing =
            detail::runProcess({"tar", "-tvJf", utf8(archive), "--numeric-owner"}, cancelled,
                               std::chrono::minutes(5), {}, 16 * 1024 * 1024);
        if (listing.error != BlenderError::None)
            return listing.error;
        if (listing.truncated)
            return BlenderError::ArchiveRejected;
        std::istringstream lines(listing.out);
        std::string line;
        std::uint64_t total = 0, count = 0;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string mode, owner, date, time, name;
            std::uint64_t size = 0;
            if (!(fields >> mode >> owner >> size >> date >> time) || ++count > 100000 ||
                mode.empty())
                return BlenderError::ArchiveRejected;
            std::getline(fields >> std::ws, name);
            if (mode[0] == 'l') {
                const auto arrow = name.rfind(" -> ");
                if (arrow == name.npos)
                    return BlenderError::ArchiveRejected;
                const auto target = pathFromUtf8(name.substr(0, arrow)).parent_path() /
                                    pathFromUtf8(name.substr(arrow + 4));
                if (!safeName(utf8(target.lexically_normal())) ||
                    !utf8(target.lexically_normal()).starts_with(release.topDirectory + "/"))
                    return BlenderError::ArchiveRejected;
                name.resize(arrow);
            } else if (mode[0] != '-' && mode[0] != 'd')
                return BlenderError::ArchiveRejected;
            if (!safeName(name) ||
                !(name == release.topDirectory || name == release.topDirectory + "/" ||
                  name.starts_with(release.topDirectory + "/")) ||
                size > 8ULL * 1024 * 1024 * 1024 - total)
                return BlenderError::ArchiveRejected;
            total += size;
        }
        if (!count || std::filesystem::space(destination).available < total)
            return BlenderError::ArchiveRejected;
        const auto result =
            detail::runProcess({"tar", "-xJf", utf8(archive), "-C", utf8(destination),
                                "--no-same-owner", "--no-same-permissions"},
                               cancelled, std::chrono::minutes(5));
        return result.error;
    }
#ifdef __APPLE__
    const auto mount = destination.parent_path() / "mount";
    std::filesystem::create_directory(mount);
    const auto attached = detail::runProcess(
        {"hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", utf8(mount), utf8(archive)},
        cancelled, std::chrono::minutes(2));
    if (attached.error != BlenderError::None)
        return attached.error;
    BlenderError error = BlenderError::None;
    try {
        std::filesystem::copy(mount / "Blender.app", destination / "Blender.app",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::copy_symlinks);
    } catch (const std::exception&) {
        error = BlenderError::IoError;
    }
    detail::runProcess({"hdiutil", "detach", utf8(mount)}, nullptr, std::chrono::minutes(1));
    return stopped(cancelled) ? BlenderError::Cancelled : error;
#else
    return BlenderError::UnsupportedPlatform;
#endif
}
} // namespace
BlenderError VerifiedBlender::validate(const std::atomic_bool* cancelled) const {
    std::error_code error;
    if (!std::filesystem::is_regular_file(m_executable, error))
        return BlenderError::CacheCorrupt;
    if (m_local) {
        const auto digest = transfer::sha256File(m_executable, cancelled);
        if (!digest)
            return stopped(cancelled) ? BlenderError::Cancelled : BlenderError::IoError;
        return *digest.value == m_digest ? BlenderError::None : BlenderError::CacheCorrupt;
    }
    const auto digest = treeHash(m_directory, cancelled);
    if (!digest)
        return digest.error;
    return *digest.value == m_digest ? BlenderError::None : BlenderError::CacheCorrupt;
}
BlenderInstaller::BlenderInstaller(std::filesystem::path cache, Download download,
                                   std::filesystem::path localExecutable, BlenderLog log)
    : m_cache(std::move(cache)), m_download(std::move(download)),
      m_localExecutable(std::move(localExecutable)), m_log(std::move(log)) {}
BlenderResult<VerifiedBlender> BlenderInstaller::ensureVersion(std::string_view version,
                                                               const std::atomic_bool* cancelled) {
    const auto platform = Platform::current();
    if (!platform)
        return {{}, BlenderError::UnsupportedPlatform};
    const auto release = officialRelease(version, *platform);
    if (!release)
        return {{}, release.error};
    if (stopped(cancelled))
        return {{}, BlenderError::Cancelled};
    if (m_cache.empty())
        return {{}, BlenderError::InvalidRequest};
    std::string stage = "preparing Blender " + std::string(version);
    const auto report = [&](std::string message) {
        stage = std::move(message);
        if (m_log)
            m_log(stage);
    };
    try {
        report("Preparing Blender " + std::string(version) + "; cache=" + utf8(m_cache));
        if (!m_localExecutable.empty()) {
            const auto executable = std::filesystem::absolute(m_localExecutable).lexically_normal();
            report("Checking selected executable: " + utf8(executable));
            if (!std::filesystem::is_regular_file(executable))
                return {{}, BlenderError::InvalidRequest, "Blender executable not found: " + utf8(executable)};
            const auto digest = transfer::sha256File(executable, cancelled);
            if (!digest)
                return {{}, stopped(cancelled) ? BlenderError::Cancelled : BlenderError::IoError,
                        "Cannot read Blender executable: " + utf8(executable)};
            const auto process = detail::runProcess({utf8(executable), "--version"}, cancelled,
                                                    std::chrono::seconds(30));
            if (process.error != BlenderError::None)
                return {{}, process.error, detail::processDiagnostic(process)};
            std::istringstream output(process.out);
            std::string product, installed;
            output >> product >> installed;
            if (product != "Blender" || !officialRelease(installed, *platform))
                return {{}, BlenderError::InvalidVersion, "Selected executable did not report a Blender version: " + process.out};
            if (installed == version) {
                report("Using selected Blender " + installed + ": " + utf8(executable));
                privateDirectory(m_cache);
                return {VerifiedBlender(executable.parent_path(), executable, std::string(version),
                                        *digest.value, std::filesystem::absolute(m_cache) / "runtime", true),
                        BlenderError::None};
            }
            report("Selected Blender is " + installed + "; using managed version " + std::string(version));
        }
        privateDirectory(m_cache);
        m_cache = std::filesystem::canonical(m_cache);
        const auto entry = m_cache / release.value->key;
        if (std::filesystem::is_symlink(entry))
            return {{}, BlenderError::CacheCorrupt};
        if (std::filesystem::exists(entry)) {
            report("Verifying cached Blender: " + utf8(entry));
            try {
                const auto manifest = entry / "manifest.json";
                if (std::filesystem::is_symlink(manifest) ||
                    std::filesystem::file_size(manifest) > 16384)
                    return {{}, BlenderError::CacheCorrupt};
                std::ifstream input(manifest);
                const auto data = Json::parse(input);
                if (data.at("format") != "blendnet-blender-v1" ||
                    data.at("version").get<std::string>() != std::string(version) ||
                    data.at("release") != release.value->key ||
                    data.at("archiveSha256").get<std::string>().size() != 64 ||
                    data.at("treeSha256").get<std::string>().size() != 64)
                    return {{}, BlenderError::CacheCorrupt};
                VerifiedBlender binary(entry / "content",
                                       entry / "content" / release.value->executable,
                                       std::string(version), data.at("treeSha256"));
                const auto valid = binary.validate(cancelled);
                return valid == BlenderError::None ? BlenderResult<VerifiedBlender>{binary, valid}
                                                   : BlenderResult<VerifiedBlender>{{}, valid};
            } catch (const std::exception&) {
                return {{}, BlenderError::CacheCorrupt};
            }
        }
        const auto locks = m_cache / "locks";
        privateDirectory(locks);
        const auto lock = locks / release.value->key;
        if (!std::filesystem::create_directory(lock))
            return {{}, BlenderError::Busy};
        Temporary lease{lock};
        const auto uuid = network::randomSessionId();
        if (!uuid)
            return {{}, BlenderError::IoError};
        const auto stage = m_cache / ("install-" + *uuid);
        privateDirectory(stage);
        Temporary cleanup{stage};
        std::string downloadDiagnostic;
        const auto download = [&](const std::string& url, const std::filesystem::path& target,
                                  std::uint64_t limit) {
            report("Downloading " + url + " to " + utf8(target));
            downloadDiagnostic.clear();
            return m_download ? m_download(url, target, limit, cancelled)
                              : downloadOfficialDiagnosed(url, target, limit, cancelled, downloadDiagnostic);
        };
        auto error = download(release.value->checksumUrl, stage / "checksums", 65536);
        if (error != BlenderError::None)
            return {{}, error, downloadDiagnostic};
        if (std::filesystem::file_size(stage / "checksums") > 65536)
            return {{}, BlenderError::ChecksumMismatch};
        std::ifstream input(stage / "checksums");
        const std::string text{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
        // Windows cannot rename the staging directory while this file is open.
        input.close();
        const auto expected = publishedChecksum(text, release.value->archiveName);
        if (!expected)
            return {{}, BlenderError::ChecksumMismatch};
        const auto downloads = m_cache / "downloads";
        privateDirectory(downloads);
        const auto archive = downloads / release.value->archiveName;
        if (std::filesystem::is_symlink(archive))
            return {{}, BlenderError::CacheCorrupt};
        if (!std::filesystem::exists(archive)) {
            const auto part = stage / "archive.partial";
            error =
                download(release.value->archiveUrl, part, 2ULL * 1024 * 1024 * 1024);
            if (error != BlenderError::None)
                return {{}, error, downloadDiagnostic};
            std::filesystem::rename(part, archive);
        }
        report("Checking archive SHA-256: " + utf8(archive));
        const auto actual = transfer::sha256File(archive, cancelled);
        if (!actual)
            return {{}, stopped(cancelled) ? BlenderError::Cancelled : BlenderError::IoError};
        if (*actual.value != *expected) {
            std::filesystem::remove(archive);
            return {{}, BlenderError::ChecksumMismatch};
        }
        const auto content = stage / "content";
        privateDirectory(content);
        report("Extracting Blender into " + utf8(content));
        error = extract(*release.value, archive, content, cancelled);
        if (error != BlenderError::None)
            return {{}, error};
        if (!std::filesystem::is_regular_file(content / release.value->executable))
            return {{}, BlenderError::ArchiveRejected};
        report("Verifying extracted Blender files");
        const auto digest = treeHash(content, cancelled);
        if (!digest)
            return {{}, digest.error};
        seal(content);
        {
            std::ofstream output(stage / "manifest.json");
            output << Json{{"format", "blendnet-blender-v1"},
                           {"version", version},
                           {"release", release.value->key},
                           {"archiveSha256", *expected},
                           {"treeSha256", *digest.value}}
                          .dump()
                   << '\n';
            output.close();
            if (!output)
                return {{}, BlenderError::IoError};
        }
        std::filesystem::permissions(stage / "manifest.json",
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
        std::filesystem::rename(stage, entry);
        cleanup.keep = true;
        report("Blender is ready: " + utf8(entry / "content" / release.value->executable));
        return {VerifiedBlender(entry / "content", entry / "content" / release.value->executable,
                                std::string(version), *digest.value),
                BlenderError::None};
    } catch (const std::exception& error) {
        return {{}, BlenderError::IoError, stage + ": " + error.what()};
    }
}
} // namespace blendnet::blender
