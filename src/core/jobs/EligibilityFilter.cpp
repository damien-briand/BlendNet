#include "jobs/EligibilityFilter.h"

#include <algorithm>

namespace blendnet::jobs {
bool EligibilityFilter::isEligible(const network::ClientInfo& client, const Job& job,
                                   const FrameRange& chunk) {
    const auto& prefs = client.preferences;
    const auto& gpu = client.gpu;
    if (chunk.count() == 0 || prefs.availability != "AVAILABLE" || prefs.maxConcurrentJobs < 1 ||
        std::find(prefs.excludedEngines.begin(), prefs.excludedEngines.end(), job.engine) !=
            prefs.excludedEngines.end() ||
        (prefs.maxFramesPerJob && chunk.count() > std::uint64_t(*prefs.maxFramesPerJob)) ||
        gpu.vramMB < job.minimumVramMB)
        return false;
    if (job.engine == "EEVEE")
        return true;
    if (job.engine != "CYCLES")
        return false;
    return gpu.backend == "CPU" ||
           ((gpu.backend == "OPTIX" || gpu.backend == "CUDA") && gpu.gpuVendor == "nvidia") ||
           (gpu.backend == "HIP" && gpu.gpuVendor == "amd") ||
           (gpu.backend == "ONEAPI" && gpu.gpuVendor == "intel") ||
           (gpu.backend == "METAL" && gpu.gpuVendor == "apple" && gpu.os == "macos");
}
} // namespace blendnet::jobs
