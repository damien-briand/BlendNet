#pragma once

#include "Cli.h"
#include "blender/Benchmark.h"
#include "blender/BlenderInstaller.h"
#include "blender/GpuInfoService.h"
#include "config/Utf8Path.h"

#include <charconv>
#include <iostream>

namespace blendnet::cli {
struct BlenderArguments {
    enum class Mode { None, Install, Probe, Benchmark, Render, Smoke } mode = Mode::None;
    std::string version;
    std::filesystem::path cache;
    std::optional<std::string> backend;
    blender::RenderRequest render;
    bool consume(std::string_view argument, std::string_view value) {
        if (argument == "--install-blender" || argument == "--probe-blender" ||
            argument == "--benchmark-blender" || argument == "--render-local" ||
            argument == "--blender-smoke") {
            if (mode != Mode::None || !version.empty())
                return false;
            mode = argument == "--install-blender"     ? Mode::Install
                   : argument == "--probe-blender"     ? Mode::Probe
                   : argument == "--benchmark-blender" ? Mode::Benchmark
                   : argument == "--render-local"      ? Mode::Render
                                                       : Mode::Smoke;
            version = value;
        } else if (argument == "--use-blender") {
            if (!version.empty())
                return false;
            version = value;
        } else if (argument == "--blender-cache")
            cache = pathFromUtf8(value);
        else if (argument == "--backend") {
            if (!blender::GpuInfoService::validBackend(value))
                return false;
            backend = value;
        } else if (argument == "--scene")
            render.scene = pathFromUtf8(value);
        else if (argument == "--output-dir")
            render.outputDirectory = pathFromUtf8(value);
        else if (argument == "--engine") {
            if (value != "CYCLES" && value != "EEVEE")
                return false;
            render.engine = value;
        } else if (argument == "--format") {
            if (value != "PNG" && value != "JPEG" && value != "EXR")
                return false;
            render.task.outputFormat = value;
        } else if (argument == "--frame-start" || argument == "--frame-end" ||
                   argument == "--frame-step" || argument == "--render-threads" ||
                   argument == "--render-timeout-sec") {
            int number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return false;
            if (argument == "--frame-start")
                render.task.frameStart = number;
            else if (argument == "--frame-end")
                render.task.frameEnd = number;
            else if (argument == "--frame-step") {
                if (number < 1)
                    return false;
                render.task.frameStep = number;
            } else if (argument == "--render-threads") {
                if (number < 1 || number > 1024)
                    return false;
                render.threads = unsigned(number);
            } else {
                if (number < 1 || number > 86400)
                    return false;
                render.timeout = std::chrono::seconds(number);
            }
        } else
            return false;
        return true;
    }
    BlenderArguments() { render.task = {"local", "local_c0", 1, 3, 1, "frame_####", "PNG"}; }
    std::filesystem::path cachePath(const AppSettings& settings,
                                    const std::filesystem::path& config) const {
        if (!cache.empty())
            return cache;
        return settings.blenderCacheDirectory.is_absolute()
                   ? settings.blenderCacheDirectory
                   : config.parent_path() / settings.blenderCacheDirectory;
    }
};
class BlenderCancellation {
  public:
    std::atomic_bool cancelled{false};
    BlenderCancellation()
        : watcher([this](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  if (interrupted) {
                      cancelled = true;
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(20));
              }
          }) {}

  private:
    std::jthread watcher;
};
inline int runBlender(const BlenderArguments& arguments, const AppSettings& settings,
                      const std::filesystem::path& config) {
    using namespace blender;
    const auto report = [](BlenderError error, const std::string& diagnostic = {}) {
        std::cerr << toString(error) << '\n';
        if (!diagnostic.empty())
            std::cerr << diagnostic << '\n';
        return 1;
    };
    if ((arguments.mode == BlenderArguments::Mode::Smoke ||
         arguments.mode == BlenderArguments::Mode::Render) &&
        arguments.render.outputDirectory.empty())
        return 2;
    handleSignals();
    BlenderCancellation control;
    const auto local = settings.blenderExecutable.empty() || settings.blenderExecutable.is_absolute()
                           ? settings.blenderExecutable : config.parent_path() / settings.blenderExecutable;
    BlenderInstaller installer(arguments.cachePath(settings, config), {}, local,
        [&](std::string_view message) {
            if (settings.logLevel == LogLevel::Debug)
                std::cerr << "BLENDER_DEBUG " << message << '\n';
        });
    std::cout << "BLENDER_VERIFYING " << arguments.version << '\n' << std::flush;
    const auto binary = installer.ensureVersion(arguments.version, &control.cancelled);
    if (!binary)
        return report(binary.error, binary.diagnostic);
    std::cout << "BLENDER_VERIFIED " << binary.value->version() << '\n' << std::flush;
    if (arguments.mode == BlenderArguments::Mode::Install)
        return 0;
    const auto backend = arguments.backend.value_or(settings.computeBackend);
    if (arguments.mode == BlenderArguments::Mode::Probe) {
        const auto hardware = GpuInfoService::probe(*binary.value, backend, &control.cancelled);
        if (!hardware)
            return report(hardware.error, hardware.diagnostic);
        std::cout << "GPU_INFO " << hardware.value->gpu.gpuModel << " | "
                  << hardware.value->gpu.gpuVendor << " | " << hardware.value->gpu.vramMB
                  << " MiB VRAM | " << hardware.value->gpu.ramMB << " MiB RAM\nBACKENDS";
        for (const auto& kind : hardware.value->availableBackends)
            std::cout << ' ' << kind;
        std::cout << '\n';
        return 0;
    }
    if (arguments.mode == BlenderArguments::Mode::Benchmark) {
        const auto result = Benchmark::run(*binary.value, backend,
                                           arguments.cachePath(settings, config) / "benchmarks",
                                           config, &control.cancelled);
        if (!result)
            return report(result.error, result.diagnostic);
        std::cout << "BENCHMARK " << result.value->score << ' ' << result.value->elapsed.count()
                  << " ms\n";
        return 0;
    }
    auto request = arguments.render;
    request.backend = backend;
    if (arguments.mode == BlenderArguments::Mode::Smoke) {
        request.scene = request.outputDirectory / "scene.blend";
        request.task = {"smoke", "smoke_c0", 1, 3, 1, "frame_####", "PNG"};
        const auto created = Benchmark::createReferenceScene(*binary.value, request.scene, false,
                                                             &control.cancelled);
        if (created != BlenderError::None)
            return report(created);
        request.outputDirectory /= "frames";
    }
    RenderProcess process;
    if (!process.start(*binary.value, request))
        return report(BlenderError::Busy);
    int lastDone = -1;
    auto lastOutput = std::chrono::steady_clock::time_point{};
    while (true) {
        if (control.cancelled)
            process.cancel();
        for (const auto& event : process.drainEvents()) {
            if (event.progress &&
                (event.progress->framesDone != lastDone ||
                 std::chrono::steady_clock::now() - lastOutput >= std::chrono::seconds(5))) {
                std::cout << "RENDER_PROGRESS " << event.progress->framesDone << ' '
                          << event.progress->framesTotal << ' ' << event.progress->percent << '\n'
                          << std::flush;
                lastDone = event.progress->framesDone;
                lastOutput = std::chrono::steady_clock::now();
            }
        }
        const auto state = process.snapshot();
        if (state.state == RenderState::Completed) {
            process.wait();
            std::cout << "RENDER_DONE "
                      << jobs::FrameRange{request.task.frameStart, request.task.frameEnd,
                                          request.task.frameStep}
                             .count()
                      << '\n';
            return 0;
        }
        if (state.state == RenderState::Failed || state.state == RenderState::Cancelled) {
            process.wait();
            return report(state.error, state.diagnostic);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
inline constexpr std::string_view blenderHelp =
    "  --install-blender VERSION | --probe-blender VERSION | --benchmark-blender VERSION\n"
    "  --render-local VERSION --scene PATH --output-dir PATH\n"
    "  --blender-smoke VERSION --output-dir PATH  Render a real three-frame CPU fixture.\n"
    "  --use-blender VERSION  Send actual GPU_INFO when connecting.\n"
    "  --blender-cache PATH --backend CPU|OPTIX|CUDA|HIP|ONEAPI|METAL\n"
    "  --frame-start N --frame-end N --frame-step N --engine CYCLES|EEVEE --format PNG|JPEG|EXR\n"
    "  --render-threads N --render-timeout-sec N\n";
} // namespace blendnet::cli
