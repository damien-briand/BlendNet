#include "blender/BlenderInstaller.h"
#include "blender/ReleaseCatalog.h"

#include <curl/curl.h>

#include <fstream>

namespace blendnet::blender {
namespace {
struct CurlRuntime {
    CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
    ~CurlRuntime() {
        if (status == CURLE_OK)
            curl_global_cleanup();
    }
};
const CurlRuntime& curlRuntime() {
    static const CurlRuntime runtime;
    return runtime;
}
struct Output {
    std::ofstream file;
    std::uint64_t limit, bytes = 0;
    const std::atomic_bool* cancelled;
};
std::size_t write(char* bytes, std::size_t size, std::size_t count, void* data) {
    auto& output = *static_cast<Output*>(data);
    if ((size && count > SIZE_MAX / size) || (output.cancelled && output.cancelled->load()))
        return 0;
    const auto total = size * count;
    if (total > output.limit - output.bytes)
        return 0;
    output.file.write(bytes, std::streamsize(total));
    if (!output.file)
        return 0;
    output.bytes += total;
    return total;
}
int progress(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* cancelled = static_cast<const std::atomic_bool*>(data);
    return cancelled && cancelled->load() ? 1 : 0;
}
} // namespace
BlenderError downloadOfficial(const std::string& url, const std::filesystem::path& target,
                              std::uint64_t limit, const std::atomic_bool* cancelled) {
    if (!officialDownloadUrl(url) || !limit)
        return BlenderError::InvalidRequest;
    if (cancelled && cancelled->load())
        return BlenderError::Cancelled;
    const auto& runtime = curlRuntime();
    if (runtime.status != CURLE_OK)
        return BlenderError::DownloadFailed;
    try {
        Output output{std::ofstream(target, std::ios::binary | std::ios::trunc), limit, 0,
                      cancelled};
        if (!output.file)
            return BlenderError::IoError;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
        if (!curl)
            return BlenderError::DownloadFailed;
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 30000L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 600000L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "BlendNet/0.1");
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &output);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, cancelled);
        const auto status = curl_easy_perform(curl.get());
        long response = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response);
        output.file.close();
        if (cancelled && cancelled->load())
            return BlenderError::Cancelled;
        if (status != CURLE_OK || response != 200 || !output.file || !output.bytes)
            return BlenderError::DownloadFailed;
        return BlenderError::None;
    } catch (const std::exception&) {
        return BlenderError::IoError;
    }
}
BlenderResult<std::string> fetchOfficialListing(const std::string& url,
                                                const std::atomic_bool* cancelled) {
    if (url != releaseRepository && (!officialDownloadUrl(url) || !url.ends_with('/')))
        return {{}, BlenderError::InvalidRequest};
    if (cancelled && cancelled->load())
        return {{}, BlenderError::Cancelled};
    if (curlRuntime().status != CURLE_OK)
        return {{}, BlenderError::DownloadFailed};
    try {
        std::string body;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
        if (!curl)
            return {{}, BlenderError::DownloadFailed};
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "BlendNet/0.1");
        const auto append = +[](char* bytes, std::size_t size, std::size_t count,
                               void* data) -> std::size_t {
            auto& text = *static_cast<std::string*>(data);
            if (size && count > SIZE_MAX / size)
                return 0;
            const auto total = size * count;
            if (total > 1024 * 1024 - text.size())
                return 0;
            try {
                text.append(bytes, total);
                return total;
            } catch (...) {
                return 0;
            }
        };
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, cancelled);
        const auto status = curl_easy_perform(curl.get());
        long response = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response);
        if (cancelled && cancelled->load())
            return {{}, BlenderError::Cancelled};
        if (status != CURLE_OK || response != 200 || body.empty())
            return {{}, BlenderError::DownloadFailed};
        return {std::move(body), BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::DownloadFailed};
    }
}
} // namespace blendnet::blender
