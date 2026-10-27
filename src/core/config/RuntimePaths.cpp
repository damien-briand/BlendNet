#include "config/RuntimePaths.h"

#include <cstdint>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace blendnet {
std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size())
        return {};
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};
    return std::filesystem::weakly_canonical(path.data()).parent_path();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path.parent_path();
#endif
}
std::filesystem::path bundledFont() {
    const auto directory = executableDirectory();
    if (directory.empty())
        return {};
    for (const auto& relative : {"../share/blendnet/fonts/DejaVuSans.ttf",
                                 "../Resources/fonts/DejaVuSans.ttf", "fonts/DejaVuSans.ttf"}) {
        const auto path = directory / relative;
        if (std::filesystem::is_regular_file(path))
            return path;
    }
    return {};
}
} // namespace blendnet
