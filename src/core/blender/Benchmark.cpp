#include "blender/Benchmark.h"
#include "blender/Process.h"
#include "blender/Scripts.h"
#include "network/Security.h"

#include <algorithm>

namespace blendnet::blender {
BlenderError Benchmark::createReferenceScene(const VerifiedBlender& binary,
                                             const std::filesystem::path& path, bool benchmark,
                                             const std::atomic_bool* cancelled) {
    try {
        if (path.extension() != ".blend" || std::filesystem::exists(path))
            return BlenderError::InvalidRequest;
        const auto target = std::filesystem::absolute(path);
        std::filesystem::create_directories(target.parent_path());
        const auto process = detail::runBlender(binary,
                                                {"-b", "--python-exit-code", "72", "--python-expr",
                                                 detail::referenceSceneScript, "--", utf8(target),
                                                 benchmark ? "benchmark" : "smoke"},
                                                cancelled, std::chrono::minutes(2));
        if (process.error != BlenderError::None)
            return process.error;
        return std::filesystem::is_regular_file(target) && std::filesystem::file_size(target) > 0
                   ? BlenderError::None
                   : BlenderError::ProcessFailed;
    } catch (const std::exception&) {
        return BlenderError::IoError;
    }
}
BlenderResult<BenchmarkResult> Benchmark::run(const VerifiedBlender& binary,
                                              std::string_view backend,
                                              const std::filesystem::path& directory,
                                              const std::filesystem::path& settingsPath,
                                              const std::atomic_bool* cancelled) {
    try {
        AppSettings settings;
        const auto loaded = AppSettings::load(settingsPath, settings);
        if (loaded != SettingsError::None && loaded != SettingsError::NotFound)
            return {{}, BlenderError::IoError};
        const auto uuid = network::randomSessionId();
        if (!uuid)
            return {{}, BlenderError::IoError};
        const auto work = directory / ("benchmark-v1-" + *uuid);
        std::filesystem::create_directories(work);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{work};
        const auto scene = work / "reference.blend";
        const auto created = createReferenceScene(binary, scene, true, cancelled);
        if (created != BlenderError::None)
            return {{}, created};
        RenderRequest request;
        request.scene = scene;
        request.outputDirectory = work / "images";
        request.backend = backend;
        request.task = {"benchmark-v1", "benchmark-v1_c0", 1, 3, 1, "frame_####", "PNG"};
        const auto rendered = RenderProcess::run(binary, request, cancelled);
        if (!rendered)
            return {{}, rendered.error};
        const auto duration = std::max<std::int64_t>(1, rendered.value->elapsed.count());
        const auto score = int(std::clamp<std::int64_t>(100000000 / duration, 1, INT32_MAX));
        if (AppSettings::update(settingsPath, [&](AppSettings& current) {
                current.computeBackend = backend;
                current.benchmarkScore = score;
                return SettingsError::None;
            }) != SettingsError::None)
            return {{}, BlenderError::IoError};
        return {BenchmarkResult{score, rendered.value->elapsed}, BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::IoError};
    }
}
} // namespace blendnet::blender
