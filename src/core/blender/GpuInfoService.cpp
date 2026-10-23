#include "blender/GpuInfoService.h"
#include "blender/Process.h"
#include "blender/Scripts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace blendnet::blender {
namespace {
int megabytes(std::uint64_t bytes) {
    return int(std::min<std::uint64_t>(bytes / (1024 * 1024), INT32_MAX));
}
int ramMB() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? megabytes(memory.ullTotalPhys) : 0;
#elif defined(__APPLE__)
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? megabytes(bytes) : 0;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? megabytes(std::uint64_t(pages) * std::uint64_t(page)) : 0;
#endif
}
std::string vendor(std::string_view backend) {
    if (backend == "OPTIX" || backend == "CUDA")
        return "nvidia";
    if (backend == "HIP")
        return "amd";
    if (backend == "ONEAPI")
        return "intel";
    if (backend == "METAL")
        return "apple";
    return "other";
}
int nvidiaVram(std::string_view name) {
#ifdef _WIN32
    const auto library = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto symbol = [&](const char* key) {
        return library ? reinterpret_cast<void*>(GetProcAddress(library, key)) : nullptr;
    };
#else
    void* library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    const auto symbol = [&](const char* key) { return library ? dlsym(library, key) : nullptr; };
#endif
    if (!library)
        return 0;
    using Init = int (*)();
    using Count = int (*)(unsigned int*);
    using Handle = int (*)(unsigned int, void**);
    struct Memory {
        unsigned long long total, free, used;
    };
    using Mem = int (*)(void*, Memory*);
    using Name = int (*)(void*, char*, unsigned int);
    const auto init = reinterpret_cast<Init>(symbol("nvmlInit_v2")),
               shutdown = reinterpret_cast<Init>(symbol("nvmlShutdown"));
    const auto count = reinterpret_cast<Count>(symbol("nvmlDeviceGetCount_v2"));
    const auto handle = reinterpret_cast<Handle>(symbol("nvmlDeviceGetHandleByIndex_v2"));
    const auto memory = reinterpret_cast<Mem>(symbol("nvmlDeviceGetMemoryInfo"));
    const auto getName = reinterpret_cast<Name>(symbol("nvmlDeviceGetName"));
    int result = 0;
    if (init && shutdown && count && handle && memory && getName && init() == 0) {
        unsigned int size = 0;
        if (count(&size) == 0 && size <= 128)
            for (unsigned int index = 0; index < size; ++index) {
                void* gpu = nullptr;
                char model[256]{};
                Memory bytes{};
                if (handle(index, &gpu) == 0 && getName(gpu, model, sizeof(model)) == 0 &&
                    memory(gpu, &bytes) == 0 && name == model)
                    result = megabytes(bytes.total);
            }
        shutdown();
    }
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return result;
}
int linuxVram(std::string_view deviceVendor) {
#ifdef __linux__
    std::error_code error;
    int found = 0, matches = 0;
    for (std::filesystem::directory_iterator it("/sys/class/drm", error), end; it != end && !error;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with("card") || name.find('-') != name.npos)
            continue;
        std::ifstream id(it->path() / "device/vendor");
        std::string value;
        id >> value;
        if ((deviceVendor == "amd" && value == "0x1002") ||
            (deviceVendor == "intel" && value == "0x8086")) {
            std::ifstream memory(it->path() / "device/mem_info_vram_total");
            std::uint64_t bytes = 0;
            if (memory >> bytes) {
                ++matches;
                found = megabytes(bytes);
            }
        }
    }
    return matches == 1 ? found : 0; // Ne pas attribuer la mémoire du mauvais GPU.
#else
    (void)deviceVendor;
    return 0;
#endif
}
} // namespace
bool GpuInfoService::validBackend(std::string_view backend) {
    return backend == "CPU" || backend == "OPTIX" || backend == "CUDA" || backend == "HIP" ||
           backend == "ONEAPI" || backend == "METAL";
}
BlenderResult<HardwareProfile> GpuInfoService::probe(const VerifiedBlender& binary,
                                                     std::string_view backend,
                                                     const std::atomic_bool* cancelled) {
    if (!validBackend(backend))
        return {{}, BlenderError::UnsupportedBackend};
    const auto process = detail::runBlender(
        binary, {"-b", "--python-exit-code", "72", "--python-expr", detail::probeScript}, cancelled,
        std::chrono::minutes(2));
    if (process.error != BlenderError::None)
        return {{},
                process.error,
                process.err.substr(process.err.size() > 4096 ? process.err.size() - 4096 : 0)};
    try {
        const auto position = process.out.rfind("BLENDNET_GPU:");
        if (position == process.out.npos)
            return {{}, BlenderError::ProcessFailed};
        const auto begin = position + std::string_view("BLENDNET_GPU:").size();
        const auto end = process.out.find_first_of("\r\n", begin);
        const auto data = nlohmann::json::parse(process.out.substr(begin, end - begin));
        if (data.at("version") != binary.version() || !data.at("devices").is_array() ||
            data.at("devices").size() > 128)
            return {{}, BlenderError::ProcessFailed};
        HardwareProfile profile;
        profile.eeveeEngine = data.value("eeveeEngine", "");
        if (profile.eeveeEngine != "" && profile.eeveeEngine != "BLENDER_EEVEE_NEXT" &&
            profile.eeveeEngine != "BLENDER_EEVEE")
            return {{}, BlenderError::ProcessFailed};
        if (data.contains("frameBounds")) {
            const auto& bounds = data.at("frameBounds");
            if (!bounds.is_array() || bounds.size() != 3)
                return {{}, BlenderError::ProcessFailed};
            profile.frameMinimum = bounds[0].get<int>();
            profile.frameMaximum = bounds[1].get<int>();
            profile.stepMaximum = bounds[2].get<int>();
        }
        for (const auto& item : data.at("devices")) {
            const auto kind = item.at("backend").get<std::string>(),
                       name = item.at("name").get<std::string>();
            if (!validBackend(kind) || name.empty() || name.size() > 1024)
                return {{}, BlenderError::ProcessFailed};
            ComputeDevice device{name, kind, vendor(kind), 0};
            if (device.vendor == "nvidia")
                device.vramMB = nvidiaVram(name);
            else if (device.vendor == "amd" || device.vendor == "intel")
                device.vramMB = linuxVram(device.vendor);
            profile.devices.push_back(device);
            if (std::find(profile.availableBackends.begin(), profile.availableBackends.end(),
                          kind) == profile.availableBackends.end())
                profile.availableBackends.push_back(kind);
        }
        const auto selected = std::find_if(profile.devices.begin(), profile.devices.end(),
                                           [&](const auto& d) { return d.backend == backend; });
        if (selected == profile.devices.end())
            return {{}, BlenderError::UnsupportedBackend};
        const auto platform = Platform::current();
        profile.gpu = {selected->name,
                       selected->vendor,
                       selected->vramMB,
                       ramMB(),
                       platform && platform->system == System::Windows ? "windows"
                       : platform && platform->system == System::MacOS ? "macos"
                                                                       : "linux",
                       binary.version(),
                       std::string(backend),
                       {}};
        return {profile, BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::ProcessFailed};
    }
}
} // namespace blendnet::blender
