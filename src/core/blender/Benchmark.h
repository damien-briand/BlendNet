#pragma once

#include "blender/RenderProcess.h"
#include "config/AppSettings.h"

namespace blendnet::blender {
struct BenchmarkResult {
    int score = 0;
    std::chrono::milliseconds elapsed{0};
};
class Benchmark {
  public:
    static BlenderError createReferenceScene(const VerifiedBlender& binary,
                                             const std::filesystem::path& path,
                                             bool benchmark = false,
                                             const std::atomic_bool* cancelled = nullptr,
                                             std::string* diagnostic = nullptr);
    static BlenderResult<BenchmarkResult> run(const VerifiedBlender& binary,
                                              std::string_view backend,
                                              const std::filesystem::path& workDirectory,
                                              const std::filesystem::path& settingsPath,
                                              const std::atomic_bool* cancelled = nullptr);
};
} // namespace blendnet::blender
