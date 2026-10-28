#pragma once

#include "blender/BlenderTypes.h"

namespace blendnet::blender::detail {
struct ProcessOutput {
    int exitCode = -1;
    BlenderError error = BlenderError::None;
    std::string out, err;
    bool truncated = false;
    std::chrono::milliseconds elapsed{0};
};
std::string processDiagnostic(const ProcessOutput& output);
// reproc uniquement, argv séparés, aucun shell. À appeler sur un worker.
ProcessOutput runProcess(const std::vector<std::string>& arguments,
                         const std::atomic_bool* cancelled, std::chrono::milliseconds timeout,
                         std::function<void(std::string_view, bool)> line = {},
                         std::size_t captureLimit = 65536,
                         const std::vector<std::string>& environment = {},
                         std::function<void()> heartbeat = {},
                         bool isolatePythonEnvironment = false);
ProcessOutput runBlender(const VerifiedBlender& binary, std::vector<std::string> arguments,
                         const std::atomic_bool* cancelled, std::chrono::milliseconds timeout,
                         std::function<void(std::string_view, bool)> line = {},
                         std::function<void()> heartbeat = {});
} // namespace blendnet::blender::detail
