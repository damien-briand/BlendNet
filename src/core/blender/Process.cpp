#include "blender/Process.h"
#include "network/Security.h"

#include <reproc/reproc.h>

#include <algorithm>
#include <array>

namespace blendnet::blender::detail {
ProcessOutput runProcess(const std::vector<std::string>& arguments,
                         const std::atomic_bool* cancelled, std::chrono::milliseconds timeout,
                         std::function<void(std::string_view, bool)> line, std::size_t captureLimit,
                         const std::vector<std::string>& environment,
                         std::function<void()> heartbeat) {
    ProcessOutput result;
    if (arguments.empty() || timeout.count() <= 0 || timeout.count() > INT32_MAX) {
        result.error = BlenderError::InvalidRequest;
        return result;
    }
    if (cancelled && cancelled->load()) {
        result.error = BlenderError::Cancelled;
        return result;
    }
    std::vector<const char*> argv, env;
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            result.error = BlenderError::InvalidRequest;
            return result;
        }
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    for (const auto& item : environment)
        env.push_back(item.c_str());
    env.push_back(nullptr);
    std::unique_ptr<reproc_t, decltype(&reproc_destroy)> process(reproc_new(), reproc_destroy);
    if (!process) {
        result.error = BlenderError::ProcessFailed;
        return result;
    }
    reproc_options options{};
    options.nonblocking = true;
    options.redirect.in.type = REPROC_REDIRECT_DISCARD;
    options.redirect.out.type = REPROC_REDIRECT_PIPE;
    options.redirect.err.type = REPROC_REDIRECT_PIPE;
    options.env.extra = env.data();
    options.stop.first = {REPROC_STOP_TERMINATE, 1000};
    options.stop.second = {REPROC_STOP_KILL, 1000};
    if (reproc_start(process.get(), argv.data(), options) < 0) {
        result.error = BlenderError::ProcessFailed;
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    auto lastHeartbeat = start;
    const auto pulse = [&] {
        if (heartbeat &&
            std::chrono::steady_clock::now() - lastHeartbeat >= std::chrono::seconds(5)) {
            try {
                heartbeat();
            } catch (const std::exception&) {
                result.error = BlenderError::ProcessFailed;
            }
            lastHeartbeat = std::chrono::steady_clock::now();
        }
    };
    std::array<bool, 2> ended{false, false};
    std::array<std::string, 2> pending;
    std::array<bool, 2> discard{false, false};
    try {
        while (result.error == BlenderError::None && (!ended[0] || !ended[1])) {
            if (cancelled && cancelled->load()) {
                result.error = BlenderError::Cancelled;
                break;
            }
            if (std::chrono::steady_clock::now() - start >= timeout) {
                result.error = BlenderError::Timeout;
                break;
            }
            reproc_event_source source{process.get(),
                                       REPROC_EVENT_OUT | REPROC_EVENT_ERR | REPROC_EVENT_EXIT, 0};
            const auto status = reproc_poll(&source, 1, 50);
            pulse();
            if (status < 0 && status != REPROC_ETIMEDOUT) {
                result.error = BlenderError::ProcessFailed;
                break;
            }
            for (int index = 0; index < 2; ++index) {
                if (ended[index])
                    continue;
                std::array<std::uint8_t, 8192> buffer{};
                const auto count =
                    reproc_read(process.get(), index == 0 ? REPROC_STREAM_OUT : REPROC_STREAM_ERR,
                                buffer.data(), buffer.size());
                if (count == REPROC_EPIPE) {
                    ended[index] = true;
                    continue;
                }
                if (count == REPROC_EWOULDBLOCK)
                    continue;
                if (count < 0) {
                    result.error = BlenderError::ProcessFailed;
                    ended[index] = true;
                    continue;
                }
                auto& output = index == 0 ? result.out : result.err;
                output.append(reinterpret_cast<const char*>(buffer.data()), std::size_t(count));
                if (output.size() > captureLimit) {
                    output.erase(0, output.size() - captureLimit);
                    result.truncated = true;
                }
                if (!line)
                    continue;
                for (int n = 0; n < count; ++n) {
                    const char c = char(buffer[std::size_t(n)]);
                    if (c == '\n' || c == '\r') {
                        if (!pending[index].empty() && !discard[index])
                            line(pending[index], index == 1);
                        pending[index].clear();
                        discard[index] = false;
                    } else if (pending[index].size() < 16384 && !discard[index])
                        pending[index] += c;
                    else {
                        pending[index].clear();
                        discard[index] = true;
                    }
                }
            }
        }
        if (line)
            for (int i = 0; i < 2; ++i)
                if (!discard[i] && !pending[i].empty())
                    line(pending[i], i == 1);
    } catch (const std::exception&) {
        result.error = BlenderError::ProcessFailed;
    }
    while (result.error == BlenderError::None) {
        pulse();
        if (cancelled && cancelled->load()) {
            result.error = BlenderError::Cancelled;
            break;
        }
        if (std::chrono::steady_clock::now() - start >= timeout) {
            result.error = BlenderError::Timeout;
            break;
        }
        result.exitCode = reproc_wait(process.get(), 50);
        if (result.exitCode == REPROC_ETIMEDOUT)
            continue;
        if (result.exitCode != 0)
            result.error = BlenderError::ProcessFailed;
        break;
    }
    if (result.error != BlenderError::None)
        reproc_stop(process.get(), options.stop);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}
ProcessOutput runBlender(const VerifiedBlender& binary, std::vector<std::string> arguments,
                         const std::atomic_bool* cancelled, std::chrono::milliseconds timeout,
                         std::function<void(std::string_view, bool)> line,
                         std::function<void()> heartbeat) {
    const auto valid = binary.validate(cancelled);
    if (valid != BlenderError::None) {
        ProcessOutput result;
        result.error = valid;
        return result;
    }
    const auto uuid = network::randomSessionId();
    if (!uuid) {
        ProcessOutput result;
        result.error = BlenderError::IoError;
        return result;
    }
    const auto runtime = binary.directory().parent_path() / "runtime" / *uuid;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{runtime};
    try {
        std::filesystem::create_directories(runtime);
        std::filesystem::permissions(runtime, std::filesystem::perms::owner_all);
        for (const auto* folder : {"config", "scripts", "extensions", "data", "cache", "tmp"})
            std::filesystem::create_directory(runtime / folder);
        std::vector<std::string> environment{"BLENDER_USER_CONFIG=" + utf8(runtime / "config"),
                                             "BLENDER_USER_SCRIPTS=" + utf8(runtime / "scripts"),
                                             "BLENDER_USER_EXTENSIONS=" +
                                                 utf8(runtime / "extensions"),
                                             "BLENDER_USER_DATAFILES=" + utf8(runtime / "data"),
                                             "XDG_CACHE_HOME=" + utf8(runtime / "cache"),
                                             "TMPDIR=" + utf8(runtime / "tmp"),
                                             "PYTHONDONTWRITEBYTECODE=1",
                                             "PYTHONNOUSERSITE=1",
                                             "PYTHONPATH=",
                                             "PYTHONHOME="};
        arguments.insert(arguments.begin(), {utf8(binary.executable()), "--factory-startup",
                                             "--disable-autoexec", "-noaudio"});
        return runProcess(arguments, cancelled, timeout, std::move(line), 65536, environment,
                          std::move(heartbeat));
    } catch (const std::exception&) {
        ProcessOutput result;
        result.error = BlenderError::IoError;
        return result;
    }
}
} // namespace blendnet::blender::detail
