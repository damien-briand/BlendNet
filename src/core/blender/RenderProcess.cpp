#include "blender/RenderProcess.h"
#include "blender/GpuInfoService.h"
#include "blender/Process.h"
#include "blender/Scripts.h"
#include "config/Utf8Path.h"
#include "jobs/ResultAssembler.h"
#include "protocol/ProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>

namespace blendnet::blender {
RenderProgressParser::RenderProgressParser(protocol::TaskAssign task, std::filesystem::path output)
    : m_task(std::move(task)), m_output(std::filesystem::absolute(output).lexically_normal()),
      m_current(m_task.frameStart) {}
std::optional<protocol::Progress> RenderProgressParser::consume(std::string_view line) {
    const jobs::FrameRange range{m_task.frameStart, m_task.frameEnd, m_task.frameStep};
    if (!range.count() || range.count() > INT32_MAX || line.size() > 16384)
        return {};
    const auto validFrame = [&](int frame) {
        return frame >= range.start && frame <= range.end &&
               (std::int64_t(frame) - range.start) % range.step == 0;
    };
    int frame = m_current;
    if (line.starts_with("Fra:")) {
        const auto parsed = std::from_chars(line.data() + 4, line.data() + line.size(), frame);
        if (parsed.ec != std::errc{} || !validFrame(frame) ||
            (parsed.ptr != line.data() + line.size() && *parsed.ptr != ' ' && *parsed.ptr != '|'))
            return {};
        if (frame == m_current && !m_completed.empty())
            return {};
        m_current = frame;
    } else if (line.starts_with("Saved: ")) {
        auto path = line.substr(7);
        if (path.size() >= 2 && ((path.front() == '\'' && path.back() == '\'') ||
                                 (path.front() == '"' && path.back() == '"')))
            path = path.substr(1, path.size() - 2);
        const auto file = pathFromUtf8(path).lexically_normal();
        if (file.parent_path() != m_output)
            return {};
        const auto name = utf8(file.filename());
        const auto dot = name.rfind('.');
        if (!name.starts_with("frame_") || dot == name.npos || dot <= 6)
            return {};
        const auto parsed = std::from_chars(name.data() + 6, name.data() + dot, frame);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + dot || !validFrame(frame) ||
            file.extension() !=
                std::filesystem::path(jobs::frameName(frame, m_task.outputFormat)).extension() ||
            !m_completed.insert(frame).second)
            return {};
        m_current = frame;
    } else
        return {};
    const int done = int(m_completed.size()), total = int(range.count());
    std::optional<int> eta;
    if (done) {
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        eta = int(std::min<double>(INT32_MAX, elapsed * (total - done) / done));
    }
    return protocol::Progress{m_task.jobId, m_task.chunkId,       m_current, done,
                              total,        100.0 * done / total, eta};
}
BlenderResult<RenderResult>
RenderProcess::run(const VerifiedBlender& binary, const RenderRequest& request,
                   const std::atomic_bool* cancelled,
                   std::function<void(const protocol::Progress&)> progress) {
    try {
        const jobs::FrameRange range{request.task.frameStart, request.task.frameEnd,
                                     request.task.frameStep};
        if (!protocol::ProtocolCodec::encode(request.task) || !range.count() ||
            range.count() > 100000 || request.task.outputPattern != "frame_####" ||
            request.outputDirectory.empty() || request.scene.empty() ||
            request.scene.extension() != ".blend" ||
            !std::filesystem::is_regular_file(request.scene) || request.threads < 1 ||
            request.threads > 1024 || request.timeout.count() <= 0 ||
            request.timeout.count() > INT32_MAX ||
            (request.engine != "CYCLES" && request.engine != "EEVEE"))
            return {{}, BlenderError::InvalidRequest};
        if (!GpuInfoService::validBackend(request.backend))
            return {{}, BlenderError::UnsupportedBackend};
        const auto hardware = GpuInfoService::probe(
            binary, request.engine == "EEVEE" ? "CPU" : request.backend, cancelled);
        if (!hardware)
            return {{}, hardware.error, hardware.diagnostic};
        if (range.start < hardware.value->frameMinimum ||
            range.end > hardware.value->frameMaximum || range.step > hardware.value->stepMaximum)
            return {{}, BlenderError::InvalidRequest};
        const auto engine = request.engine == "CYCLES" ? "CYCLES" : hardware.value->eeveeEngine;
        if (engine.empty())
            return {{}, BlenderError::UnsupportedBackend};
        const auto output = std::filesystem::absolute(request.outputDirectory).lexically_normal();
        std::filesystem::create_directories(output);
        for (std::int64_t n = range.start; n <= range.end; n += range.step)
            if (std::filesystem::exists(output /
                                        jobs::frameName(int(n), request.task.outputFormat)))
                return {{}, BlenderError::InvalidRequest};
        RenderProgressParser parser(request.task, output);
        protocol::Progress lastProgress{
            request.task.jobId, request.task.chunkId, range.start, 0, int(range.count()), 0, {}};
        const auto parameters =
            nlohmann::json{{"start", range.start}, {"end", range.end}, {"step", range.step}}.dump();
        const auto process = detail::runBlender(
            binary,
            {"-b",
             utf8(std::filesystem::absolute(request.scene)),
             "-E",
             engine,
             "-s",
             std::to_string(range.start),
             "-e",
             std::to_string(range.end),
             "-j",
             std::to_string(range.step),
             "-o",
             utf8(output / "frame_####"),
             "-F",
             request.task.outputFormat,
             "-x",
             "1",
             "-t",
             std::to_string(request.threads),
             "--python-exit-code",
             "72",
             "--python-expr",
             detail::configureRenderScript,
             "-a",
             "--",
             "--cycles-device",
             request.backend,
             "--blendnet-request",
             parameters},
            cancelled, request.timeout,
            [&](std::string_view line, bool stderrStream) {
                if (stderrStream)
                    return;
                if (const auto value = parser.consume(line); value) {
                    lastProgress = *value;
                    if (progress)
                        progress(*value);
                }
            },
            [&] {
                if (progress)
                    progress(lastProgress);
            });
        if (process.error != BlenderError::None)
            return {{}, process.error, detail::processDiagnostic(process)};
        RenderResult result;
        result.elapsed = process.elapsed;
        std::uint32_t width = 0, height = 0;
        for (std::int64_t n = range.start; n <= range.end; n += range.step) {
            if (cancelled && cancelled->load())
                return {{}, BlenderError::Cancelled};
            const auto image = jobs::ResultAssembler::inspect(
                int(n), output / jobs::frameName(int(n), request.task.outputFormat),
                request.task.outputFormat, cancelled);
            if (!image)
                return {{}, BlenderError::MissingFrames};
            if (!width) {
                width = image->width;
                height = image->height;
            }
            if (image->width != width || image->height != height)
                return {{}, BlenderError::MissingFrames};
            result.frames.push_back(*image);
        }
        return {result, BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::IoError};
    }
}
void RenderProcess::publish(RenderSnapshot state) {
    const std::lock_guard lock(m_mutex);
    m_snapshot = std::move(state);
    if (m_events.size() == 1024)
        m_events.erase(m_events.begin());
    m_events.push_back(m_snapshot);
}
RenderProcess::~RenderProcess() {
    cancel();
    wait();
}
bool RenderProcess::start(const VerifiedBlender& binary, const RenderRequest& request) {
    if (m_running.exchange(true))
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancelled = false;
    {
        const std::lock_guard lock(m_mutex);
        m_events.clear();
        m_snapshot = {};
    }
    publish({RenderState::Validating});
    m_thread = std::jthread([this, binary, request] {
        const auto result =
            run(binary, request, &m_cancelled, [this](const protocol::Progress& progress) {
                publish({RenderState::Rendering, BlenderError::None, progress});
            });
        auto state = snapshot();
        state.error = result.error;
        state.diagnostic = result.diagnostic;
        state.state = result                                    ? RenderState::Completed
                      : result.error == BlenderError::Cancelled ? RenderState::Cancelled
                                                                : RenderState::Failed;
        if (result)
            state.elapsed = result.value->elapsed;
        publish(state);
        m_running = false;
    });
    return true;
}
void RenderProcess::cancel() { m_cancelled = true; }
void RenderProcess::wait() {
    if (m_thread.joinable())
        m_thread.join();
}
RenderSnapshot RenderProcess::snapshot() const {
    const std::lock_guard lock(m_mutex);
    return m_snapshot;
}
std::vector<RenderSnapshot> RenderProcess::drainEvents() {
    const std::lock_guard lock(m_mutex);
    std::vector<RenderSnapshot> result;
    result.swap(m_events);
    return result;
}
} // namespace blendnet::blender
