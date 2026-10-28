#include "Views.h"
#include "I18n.h"
#include "config/Utf8Path.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <imgui_stdlib.h>
#include <iomanip>
#include <sstream>

namespace blendnet::ui {
namespace {
std::string pathText(const std::filesystem::path& p) { return blender::utf8(p); }
std::string label(const char* text, const std::string& id) {
    return std::string(text) + "###" + id;
}
void value(std::string_view text) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}
void wrapped(std::string_view text) { ImGui::TextWrapped("%.*s", int(text.size()), text.data()); }
std::string bytes(std::uint64_t n) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(1) << double(n) / (1024 * 1024) << " MiB";
    return s.str();
}
std::string chunkName(std::string_view id) {
    const auto pos = id.rfind("_c");
    int number = 0;
    if (pos != id.npos &&
        std::from_chars(id.data() + pos + 2, id.data() + id.size(), number).ec == std::errc{} &&
        number >= 0 && number < 1000000)
        return "#" + std::to_string(number + 1);
    return std::string(id.substr(0, 12));
}
std::string date(std::filesystem::file_time_type time) {
#ifdef _MSVC_STL_VERSION
    // MSVC exposes file_clock::to_utc instead of to_sys; clock_cast handles
    // the conversion through UTC, including its leap-second adjustment.
    const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(time);
#else
    const auto systemTime = std::chrono::file_clock::to_sys(time);
#endif
    const auto t = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M");
    return out.str();
}
std::string_view chunkState(jobs::Chunk::State state) {
    constexpr std::string_view names[] = {"Pending",   "Assigned", "Downloading", "Rendering",
                                          "Uploading", "Done",     "Failed"};
    return names[int(state)];
}
std::string_view connectionState(network::ConnectionState state) {
    constexpr std::string_view names[] = {"Disconnected", "Connecting",   "Authenticating",
                                          "Ready",        "Reconnecting", "Stopped"};
    return names[int(state)];
}
} // namespace
Views::Views(application::DesktopController& controller, Automation& test)
    : m_controller(controller), m_test(test) {
    m_state = controller.snapshot();
    m_edit = m_state.settings;
    m_export = "blendnet-events.log";
}
const char* Views::tr(std::string_view key) const { return translate(key, m_edit.language).data(); }
bool Views::button(std::string_view key, const std::string& id, bool enabled, ImVec2 size) {
    const bool selected =
        (id == "nav.projects" && m_page == 0) || (id == "nav.job" && m_page == 1) ||
        (id == "nav.clients" && m_page == 2) || (id == "nav.settings" && m_page == 3) ||
        (id == "nav.renders" && m_page == 4);
    if (selected)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.14F, .34F, .34F, 1));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label(tr(key), id).c_str(), size);
    m_test.item(id, enabled);
    ImGui::EndDisabled();
    if (selected)
        ImGui::PopStyleColor();
    return pressed;
}
bool Views::input(std::string_view key, const std::string& id, std::string& text,
                  ImGuiInputTextFlags flags) {
    value(tr(key));
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputText(label("", id).c_str(), &text, flags);
    m_test.item(id);
    return changed;
}
void Views::pathInput(std::string_view key, const std::string& id, std::filesystem::path& path) {
    auto text = pathText(path);
    if (input(key, id, text))
        path = pathFromUtf8(text);
}
void Views::heading(std::string_view title) {
    ImGui::Spacing();
    ImGui::TextColored({.39F, .85F, .77F, 1}, "%s", tr(title));
    ImGui::Separator();
    ImGui::Spacing();
}
void Views::draw(GLFWwindow* window) {
    const bool awaitingVersion = m_state.settings.blenderVersion.empty();
    m_state = m_controller.snapshot();
    const auto observed = std::chrono::steady_clock::now();
    for (const auto& job : m_state.server.jobs) {
        m_started.try_emplace(job.job.id, observed);
        if (job.job.state == jobs::Job::State::Done || job.job.state == jobs::Job::State::Cancelled)
            m_finished.try_emplace(job.job.id, observed);
    }
    for (const auto& offer : m_state.offers)
        m_projectNames[offer.job.jobId] = offer.job.projectName;
    for (const auto& client : m_state.server.clients)
        m_clientNames[client.id] = client.machineName;
    while (m_projectNames.size() > 512)
        m_projectNames.erase(m_projectNames.begin());
    while (m_clientNames.size() > 512)
        m_clientNames.erase(m_clientNames.begin());
    if (m_revision != m_state.settingsRevision) {
        m_revision = m_state.settingsRevision;
        // Resolving the initial version must preserve edits already made in the form.
        if (awaitingVersion && !m_state.settings.blenderVersion.empty()) {
            if (m_edit.blenderVersion.empty())
                m_edit.blenderVersion = m_state.settings.blenderVersion;
        } else
            m_edit = m_state.settings;
    }
    for (auto& source : m_edit.knownSources)
        for (const auto& saved : m_state.settings.knownSources)
            if (source.host == saved.host && source.port == saved.port &&
                source.token == saved.token)
                source.certificateSha256 = saved.certificateSha256;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("BlendNet", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("navigation", {184, 0}, ImGuiChildFlags_Borders);
    ImGui::TextColored({.39F, .85F, .77F, 1}, "BLENDNET");
    ImGui::Spacing();
    wrapped(tr("Subtitle"));
    ImGui::Spacing();
    ImGui::Separator();
    if (button("Source", "role.source", true, {76, 32})) {
        m_client = false;
        m_page = 0;
        m_settingsTab = 2;
    }
    ImGui::SameLine();
    if (button("Client", "role.client", true, {76, 32})) {
        m_client = true;
        m_page = 4;
        m_settingsTab = 1;
    }
    ImGui::Spacing();
    if (button("Projects", "nav.projects", true, {-1, 38}))
        m_page = 0;
    if (button("Current job", "nav.job", true, {-1, 38}))
        m_page = 1;
    if (button("Clients", "nav.clients", true, {-1, 38}))
        m_page = 2;
    if (button("My renders", "nav.renders", true, {-1, 38}))
        m_page = 4;
    if (button("Settings", "nav.settings", true, {-1, 38})) {
        m_page = 3;
        m_settingsTab = m_client ? 1 : 2;
    }
    ImGui::Spacing();
    ImGui::Separator();
    value(m_state.listening ? tr("Listening") : tr("Stopped"));
    if (m_state.listening)
        ImGui::Text("TCP %u", m_state.server.port);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("workspace", {0, 0});
    heading("Workspace");
    ImGui::TextWrapped("%s", tr(m_state.status));
    if (!m_state.detail.empty())
        wrapped(tr(m_state.detail));
    if (m_state.busy) {
        ImGui::SameLine();
        if (button("Cancel operation", "operation.cancel"))
            m_controller.cancelOperation();
    }
    ImGui::Separator();
    if (m_page == 0)
        library();
    else if (m_page == 1)
        progress();
    else if (m_page == 2)
        clients();
    else if (m_page == 3)
        settings();
    else
        clientJobs();
    ImGui::EndChild();
    ImGui::End();
    popups(window);
    m_test.marker("wait.idle", !m_state.busy);
    m_test.marker("wait.projects", !m_state.projects.empty());
    m_test.marker("wait.project", m_state.selected.has_value() && !m_state.busy);
    m_test.marker("wait.server", m_state.listening);
    m_test.marker("wait.clients", !m_state.server.clients.empty());
    m_test.marker("wait.connected",
                  std::any_of(m_state.sources.begin(), m_state.sources.end(), [](const auto& s) {
                      return s.connection.state == network::ConnectionState::Ready;
                  }));
    m_test.marker("wait.done", std::any_of(m_state.server.jobs.begin(), m_state.server.jobs.end(),
                                           [](const auto& j) {
                                               return j.job.state == jobs::Job::State::Done &&
                                                      j.validFrames == j.totalFrames;
                                           }));
    m_test.marker(
        "wait.completed",
        std::any_of(m_state.clientHistory.begin(), m_state.clientHistory.end(),
                    [](const auto& e) { return e.state == jobs::ExecutorState::Completed; }));
    m_test.marker("wait.twoChunks", std::count_if(m_state.clientHistory.begin(),
                                                  m_state.clientHistory.end(), [](const auto& e) {
                                                      return e.state ==
                                                             jobs::ExecutorState::Completed;
                                                  }) >= 2);
}
void Views::library() {
    const float width = std::min(350.F, ImGui::GetContentRegionAvail().x * .38F);
    ImGui::BeginChild("library", {width, 0}, ImGuiChildFlags_Borders);
    heading("Library");
    ImGui::SetNextItemWidth(-1);
    input("Folder", "library.folder", m_folder);
    if (button("Add folder", "library.add", !m_state.busy && !m_folder.empty())) {
        auto next = m_edit;
        next.projectDirectories.push_back(pathFromUtf8(m_folder));
        m_controller.saveSettings(std::move(next));
    }
    ImGui::SameLine();
    if (button("Scan", "library.scan", !m_state.busy))
        m_controller.scan();
    if (m_state.projects.empty())
        wrapped(tr("No projects"));
    ImGuiListClipper rows;
    rows.Begin(int(m_state.projects.size()));
    while (rows.Step())
        for (int row = rows.DisplayStart; row < rows.DisplayEnd; ++row) {
            const auto i = std::size_t(row);
            const auto& p = m_state.projects[i];
            ImGui::PushID(int(i));
            ImGui::BeginDisabled(m_state.busy);
            const auto id = "project." + std::to_string(i);
            if (ImGui::Selectable((p.name + "###project").c_str(),
                                  m_selected == pathText(p.path))) {
                m_selected = pathText(p.path);
                m_controller.inspect(p.path);
            }
            m_test.item(id, !m_state.busy);
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", date(p.modified).c_str());
            ImGui::TextDisabled("%s", pathText(p.path).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", pathText(p.path).c_str());
            if (button("Remove", "project.remove." + std::to_string(i), !m_state.busy)) {
                auto next = m_edit;
                next.hiddenProjects.push_back(p.path);
                m_controller.saveSettings(std::move(next));
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("detail", {0, 0}, ImGuiChildFlags_Borders);
    project();
    ImGui::EndChild();
}
void Views::project() {
    heading("Details");
    if (!m_state.selected ||
        (!m_selected.empty() && pathText(m_state.selected->entry.path) != m_selected)) {
        wrapped(tr("Choose project"));
        return;
    }
    const auto& p = *m_state.selected;
    if (m_prepared != pathText(p.entry.path)) {
        m_prepared = pathText(p.entry.path);
        m_draft.engine = p.engine == "EEVEE" ? "EEVEE" : "CYCLES";
        m_draft.frameStart = p.start;
        m_draft.frameEnd = p.end;
        m_draft.frameStep = p.step;
        m_draft.outputFormat = (p.format == "OPEN_EXR" || p.format == "OPEN_EXR_MULTILAYER") ? "EXR"
                               : p.format == "JPEG" ? "JPEG"
                                                    : "PNG";
        m_output = pathText(m_edit.workDirectory / "renders" / p.entry.path.stem());
    }
    wrapped(p.entry.name);
    ImGui::TextDisabled("%s", pathText(p.entry.path).c_str());
    ImGui::Text("%s: %d x %d", tr("Resolution"), p.width, p.height);
    ImGui::Text("%s: %s", tr("Engine"), p.engine.c_str());
    ImGui::Text("%s: %zu   |   %s: %s", tr("Dependencies"), p.dependencies.size(), tr("Estimate"),
                bytes(p.estimatedBytes).c_str());
    for (const auto& dep : p.dependencies)
        ImGui::BulletText("%s", pathText(dep).c_str());
    ImGui::Spacing();
    heading("Share Render");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt(tr("Start"), &m_draft.frameStart, 0, 0);
    m_test.item("job.start");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt(tr("End"), &m_draft.frameEnd, 0, 0);
    m_test.item("job.end");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt(tr("Step"), &m_draft.frameStep, 0, 0);
    m_test.item("job.step");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt(tr("Chunk size"), &m_draft.chunkSize, 0, 0);
    m_test.item("job.chunk");
    int engine = m_draft.engine == "CYCLES" ? 0 : 1;
    if (ImGui::Combo(tr("Engine"), &engine, "Cycles\0EEVEE\0"))
        m_draft.engine = engine == 0 ? "CYCLES" : "EEVEE";
    int format = m_draft.outputFormat == "PNG" ? 0 : m_draft.outputFormat == "JPEG" ? 1 : 2;
    if (ImGui::Combo(tr("Format"), &format, "PNG\0JPEG\0EXR\0"))
        m_draft.outputFormat = format == 0 ? "PNG" : format == 1 ? "JPEG" : "EXR";
    input("Additional dependencies", "job.dependencies", m_extra);
    input("Output folder", "job.output", m_output);
    wrapped(tr("Portable project"));
    const auto count =
        jobs::FrameRange{m_draft.frameStart, m_draft.frameEnd, m_draft.frameStep}.count();
    if (button("Share Render", "job.share",
               m_state.listening && !m_state.busy && count > 0 && m_draft.chunkSize > 0 &&
                   !m_output.empty(),
               {-1, 42})) {
        m_draft.outputDirectory = pathFromUtf8(m_output);
        m_draft.dependencies =
            m_extra.empty() ? std::vector<std::string>{} : std::vector<std::string>{m_extra};
        m_controller.share(m_draft);
        m_page = 1;
    }
    if (!m_state.listening)
        wrapped(tr("Start source first"));
}
void Views::progress() {
    heading("Current job");
    if (m_state.server.jobs.empty()) {
        wrapped(tr("No jobs"));
        return;
    }
    m_job = std::clamp(m_job, 0, int(m_state.server.jobs.size()) - 1);
    if (ImGui::BeginCombo(tr("Project"), m_state.server.jobs[m_job].job.projectName.c_str())) {
        for (std::size_t i = 0; i < m_state.server.jobs.size(); ++i)
            if (ImGui::Selectable(m_state.server.jobs[i].job.projectName.c_str(), int(i) == m_job))
                m_job = int(i);
        ImGui::EndCombo();
    }
    const auto& j = m_state.server.jobs[m_job];
    value(tr(jobs::toString(j.job.state)));
    ImGui::ProgressBar(j.totalFrames ? float(j.validFrames) / float(j.totalFrames) : 0, {-1, 26});
    ImGui::Text("%s: %llu / %llu", tr("Validated"), static_cast<unsigned long long>(j.validFrames),
                static_cast<unsigned long long>(j.totalFrames));
    const auto now = std::chrono::steady_clock::now();
    auto [start, inserted] = m_started.try_emplace(j.job.id, now);
    (void)inserted;
    if (j.job.state == jobs::Job::State::Done || j.job.state == jobs::Job::State::Cancelled)
        m_finished.try_emplace(j.job.id, now);
    const auto end = m_finished.contains(j.job.id) ? m_finished.at(j.job.id) : now;
    const double seconds = std::max(1., std::chrono::duration<double>(end - start->second).count());
    const double rate = double(j.validFrames) / seconds;
    ImGui::Text("%s: %.2f frames/s  |  %s: %.0f s", tr("Rate"), rate, tr("ETA"),
                rate ? (j.totalFrames - j.validFrames) / rate : 0.);
    if (button(j.job.state == jobs::Job::State::Paused ? "Resume" : "Pause", "job.pause",
               j.job.state == jobs::Job::State::Paused ||
                   j.job.state == jobs::Job::State::Rendering ||
                   j.job.state == jobs::Job::State::Recruiting))
        m_controller.jobAction(j.job.id,
                               j.job.state == jobs::Job::State::Paused ? "resume" : "pause");
    ImGui::SameLine();
    if (button("Cancel job", "job.cancel",
               j.job.state != jobs::Job::State::Done && j.job.state != jobs::Job::State::Cancelled))
        m_controller.jobAction(j.job.id, "cancel");
    if (!j.failures.empty() && button("Finalize", "job.finalize"))
        ImGui::OpenPopup(tr("Finalize"));
    if (ImGui::BeginPopupModal(tr("Finalize"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(tr("Confirm failures"));
        for (const auto& id : j.failures)
            value(id);
        if (button("Confirm", "job.finalize.confirm")) {
            m_controller.jobAction(j.job.id, "finalize");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Back", "job.finalize.back"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginTable("chunks", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          {0, 220})) {
        for (const auto* key : {"Chunk", "Frames", "Client", "State", "Progress"})
            ImGui::TableSetupColumn(tr(key));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(j.chunks.size()));
        while (clipper.Step())
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto& c = j.chunks[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                value(chunkName(c.id));
                ImGui::TableNextColumn();
                ImGui::Text("%d - %d / %d", c.frameStart, c.frameEnd, c.frameStep);
                ImGui::TableNextColumn();
                value(m_clientNames.contains(c.assignedClientId)
                          ? m_clientNames.at(c.assignedClientId)
                          : c.assignedClientId.substr(0, 12));
                ImGui::TableNextColumn();
                value(tr(chunkState(c.state)));
                ImGui::TableNextColumn();
                ImGui::Text("%d / %llu (%s: %d)", c.framesDone,
                            static_cast<unsigned long long>(
                                jobs::FrameRange{c.frameStart, c.frameEnd, c.frameStep}.count()),
                            tr("Attempts"), c.attempts);
            }
        ImGui::EndTable();
    }
    logs();
}
void Views::clients() {
    heading("Clients");
    if (m_state.server.clients.empty())
        wrapped(tr("No clients"));
    if (ImGui::BeginTable("clients", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_ScrollX)) {
        for (const auto* key :
             {"Name", "Machine", "Hardware", "Backend", "State", "Progress", "Client"})
            ImGui::TableSetupColumn(tr(key));
        ImGui::TableHeadersRow();
        for (const auto& c : m_state.server.clients) {
            ImGui::PushID(c.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            value(c.userName);
            ImGui::TableNextColumn();
            value(c.machineName);
            ImGui::TableNextColumn();
            value(c.gpu.gpuModel);
            ImGui::Text("%d MiB", c.gpu.vramMB);
            ImGui::TableNextColumn();
            value(c.gpu.backend);
            const jobs::Chunk* task = nullptr;
            for (const auto& j : m_state.server.jobs)
                for (const auto& chunk : j.chunks)
                    if (chunk.assignedClientId == c.id && chunk.state != jobs::Chunk::State::Done &&
                        chunk.state != jobs::Chunk::State::Failed)
                        task = &chunk;
            ImGui::TableNextColumn();
            value(task ? tr(chunkState(task->state)) : tr("Idle"));
            ImGui::TableNextColumn();
            if (task)
                ImGui::Text("%d / %llu", task->framesDone,
                            static_cast<unsigned long long>(
                                jobs::FrameRange{task->frameStart, task->frameEnd, task->frameStep}
                                    .count()));
            ImGui::TableNextColumn();
            if (button("Cancel chunk", "client.cancel." + c.id, task))
                m_controller.clientAction(c.id, false);
            if (button("Exclude", "client.exclude." + c.id, !m_state.server.jobs.empty()))
                m_controller.excludeClient(
                    c.id,
                    m_state.server.jobs[std::clamp(m_job, 0, int(m_state.server.jobs.size()) - 1)]
                        .job.id);
            if (button("Disconnect", "client.disconnect." + c.id))
                m_controller.clientAction(c.id, true);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    logs();
}
void Views::settings() {
    heading("Settings");
    for (int i = 0; i < 3; ++i) {
        if (i)
            ImGui::SameLine();
        const char* key = i == 0 ? "General" : i == 1 ? "Client" : "Server";
        if (button(key, std::string("settings.") + (i == 0   ? "general"
                                                    : i == 1 ? "client"
                                                             : "server")))
            m_settingsTab = i;
    }
    ImGui::SameLine();
    if (button("Save", "settings.save", !m_state.busy))
        m_controller.saveSettings(m_edit);
    ImGui::BeginChild("settings.scroll", {0, 0});
    if (m_settingsTab == 0) {
        heading("General");
        if (ImGui::RadioButton("Français", m_edit.language == "fr"))
            m_edit.language = "fr";
        m_test.item("language.fr");
        ImGui::SameLine();
        if (ImGui::RadioButton("English", m_edit.language == "en"))
            m_edit.language = "en";
        m_test.item("language.en");
        pathInput("Work folder", "settings.work", m_edit.workDirectory);
        pathInput("Blender cache", "settings.cache", m_edit.blenderCacheDirectory);
        pathInput("Blender executable", "settings.executable", m_edit.blenderExecutable);
        wrapped(tr("Blender executable help"));
        if (ImGui::BeginCombo(tr("Log level"), toString(m_edit.logLevel).data())) {
            for (const auto level : {LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error, LogLevel::Off})
                if (ImGui::Selectable(toString(level).data(), m_edit.logLevel == level))
                    m_edit.logLevel = level;
            ImGui::EndCombo();
        }
        wrapped(tr("Debug logs help"));
        value(tr("Blender version"));
        ImGui::SetNextItemWidth(-1);
        const auto* preview = m_edit.blenderVersion.empty() ? tr("versions.automatic")
                                                           : m_edit.blenderVersion.c_str();
        const bool versionsOpen = ImGui::BeginCombo(label("", "settings.version").c_str(), preview);
        m_test.item("settings.version");
        if (versionsOpen) {
            if (ImGui::Selectable(tr("versions.automatic"), m_edit.blenderVersion.empty()))
                m_edit.blenderVersion.clear();
            if (!m_edit.blenderVersion.empty() &&
                std::find(m_state.blenderVersions.begin(), m_state.blenderVersions.end(),
                          m_edit.blenderVersion) == m_state.blenderVersions.end()) {
                ImGui::Selectable(m_edit.blenderVersion.c_str(), true);
            }
            for (const auto& version : m_state.blenderVersions)
                if (ImGui::Selectable(version.c_str(), m_edit.blenderVersion == version))
                    m_edit.blenderVersion = version;
            ImGui::EndCombo();
        }
        if (button("versions.refresh", "settings.versions.refresh", !m_state.blenderVersionsLoading))
            m_controller.refreshBlenderVersions();
        if (m_state.blenderVersionsLoading)
            wrapped(tr("versions.loading"));
        else if (m_state.blenderVersionsFailed)
            wrapped(tr("versions.failed"));
        if (m_state.hardware) {
            value(m_state.hardware->gpu.gpuModel);
            ImGui::Text("RAM %d MiB / VRAM %d MiB", m_state.hardware->gpu.ramMB,
                        m_state.hardware->gpu.vramMB);
        } else
            wrapped(tr("Not probed"));
        if (ImGui::BeginCombo(tr("Backend"), m_edit.computeBackend.c_str())) {
            const auto backends = m_state.hardware ? m_state.hardware->availableBackends
                                                   : std::vector<std::string>{"CPU"};
            for (const auto& backend : backends)
                if (ImGui::Selectable(backend.c_str(), m_edit.computeBackend == backend))
                    m_edit.computeBackend = backend;
            ImGui::EndCombo();
        }
        const bool saved = m_edit == m_state.settings;
        const bool versionReady = !m_state.settings.blenderVersion.empty();
        if (button("Probe", "hardware.probe", !m_state.busy && saved && versionReady))
            m_controller.probe();
        ImGui::SameLine();
        if (button("Benchmark", "hardware.benchmark", !m_state.busy && saved && versionReady))
            m_controller.benchmark();
        if (!saved)
            wrapped(tr("Save first"));
        if (m_state.settings.benchmarkScore)
            ImGui::Text("Score: %d", *m_state.settings.benchmarkScore);
    } else if (m_settingsTab == 1) {
        heading("Known sources");
        if (button("Add source", "source.add")) {
            m_edit.knownSources.push_back({"127.0.0.1", 47050, "", {}});
            m_source = int(m_edit.knownSources.size()) - 1;
        }
        if (!m_edit.knownSources.empty()) {
            m_source = std::clamp(m_source, 0, int(m_edit.knownSources.size()) - 1);
            if (ImGui::BeginCombo(tr("Source"), m_edit.knownSources[m_source].host.c_str())) {
                for (std::size_t i = 0; i < m_edit.knownSources.size(); ++i)
                    if (ImGui::Selectable((m_edit.knownSources[i].host + ":" +
                                           std::to_string(m_edit.knownSources[i].port))
                                              .c_str(),
                                          int(i) == m_source))
                        m_source = int(i);
                ImGui::EndCombo();
            }
            auto& s = m_edit.knownSources[m_source];
            input("Host", "source.host", s.host);
            ImGui::InputInt(tr("Port"), &s.port, 0, 0);
            m_test.item("source.port");
            input("Token", "source.token", s.token, m_reveal ? 0 : ImGuiInputTextFlags_Password);
            ImGui::Checkbox(tr("Reveal"), &m_reveal);
            const auto node =
                std::find_if(m_state.sources.begin(), m_state.sources.end(),
                             [&](const auto& item) { return int(item.index) == m_source; });
            if (node != m_state.sources.end()) {
                value(tr(connectionState(node->connection.state)));
                if (node->connection.error != network::NetworkError::None &&
                    node->connection.error != network::NetworkError::Stopped)
                    wrapped(tr(network::toString(node->connection.error)));
            }
            if (button("Connect", "source.connect",
                       !m_state.busy && m_edit.knownSources == m_state.settings.knownSources))
                m_controller.connectSource(std::size_t(m_source));
            ImGui::SameLine();
            if (button("Disconnect", "source.disconnect", node != m_state.sources.end()))
                m_controller.disconnectSource(std::size_t(m_source));
            ImGui::SameLine();
            if (button("Remove", "source.remove", node == m_state.sources.end()))
                m_edit.knownSources.erase(m_edit.knownSources.begin() + m_source);
        }
        heading("Availability");
        if (ImGui::BeginCombo(tr("Availability"), tr(m_edit.availability))) {
            for (const char* key : {"AVAILABLE", "BUSY_GAME", "BUSY_WORK", "DND", "OFFLINE"})
                if (ImGui::Selectable(tr(key), m_edit.availability == key))
                    m_edit.availability = key;
            ImGui::EndCombo();
        }
        for (const char* engine : {"CYCLES", "EEVEE"}) {
            bool excluded = std::find(m_edit.excludedEngines.begin(), m_edit.excludedEngines.end(),
                                      engine) != m_edit.excludedEngines.end();
            if (ImGui::Checkbox(
                    tr(std::string(engine) == "CYCLES" ? "Exclude Cycles" : "Exclude EEVEE"),
                    &excluded)) {
                std::erase(m_edit.excludedEngines, engine);
                if (excluded)
                    m_edit.excludedEngines.push_back(engine);
            }
        }
        ImGui::Checkbox(tr("Schedule"), &m_edit.scheduleEnabled);
        if (m_edit.scheduleEnabled) {
            ImGui::SliderInt(tr("From hour"), &m_edit.availableFromHour, 0, 23);
            ImGui::SliderInt(tr("Until hour"), &m_edit.availableUntilHour, 0, 23);
            wrapped(tr("Schedule hint"));
        }
        ImGui::InputInt(tr("Max frames"), &m_edit.maxFramesPerJob);
        ImGui::InputInt(tr("Quota"), &m_edit.quotaGB);
        ImGui::InputInt(tr("Threads"), &m_edit.renderThreads);
        ImGui::InputInt(tr("Timeout"), &m_edit.renderTimeoutSec);
        ImGui::InputInt(tr("Offer timeout"), &m_edit.offerTimeoutSec);
        ImGui::Checkbox(tr("Background"), &m_edit.backgroundRendering);
        ImGui::Checkbox(tr("Activity"), &m_edit.stopOnActivity);
        ImGui::Checkbox(tr("Retention"), &m_edit.keepCompletedJobs);
    } else {
        heading("Server");
        ImGui::BeginDisabled(m_state.listening);
        ImGui::InputInt(tr("Port"), &m_edit.serverPort);
        m_test.item("server.port", !m_state.listening);
        ImGui::EndDisabled();
        input("Public address", "server.address", m_edit.publicAddress);
        wrapped(tr("Routing hint"));
        if (button("Start server", "server.start",
                   !m_state.busy && !m_state.listening &&
                       m_edit.serverPort == m_state.settings.serverPort))
            m_controller.startServer();
        ImGui::SameLine();
        if (button("Stop server", "server.stop", m_state.listening))
            m_controller.stopServer();
        std::string token = m_state.pairingToken;
        input("Token", "server.token", token,
              ImGuiInputTextFlags_ReadOnly | (m_reveal ? 0 : ImGuiInputTextFlags_Password));
        ImGui::Checkbox(tr("Reveal"), &m_reveal);
        if (button("Copy token", "server.copy", !token.empty()))
            ImGui::SetClipboardText(token.c_str());
        ImGui::SameLine();
        if (button("Regenerate", "server.regenerate", !m_state.listening && !m_state.busy))
            m_controller.regenerateToken();
        logs();
    }
    ImGui::EndChild();
}
void Views::clientJobs() {
    heading("My renders");
    if (m_state.sources.empty())
        wrapped(tr("No active render"));
    for (const auto& source : m_state.sources) {
        if (source.index < m_state.settings.knownSources.size())
            value(m_state.settings.knownSources[source.index].host);
        value(tr(connectionState(source.connection.state)));
        if (source.connection.error != network::NetworkError::None &&
            source.connection.error != network::NetworkError::Stopped)
            wrapped(tr(network::toString(source.connection.error)));
        if (source.progress) {
            const auto& p = *source.progress;
            value(m_projectNames.contains(p.jobId) ? m_projectNames.at(p.jobId)
                                                   : p.jobId.substr(0, 12));
            ImGui::ProgressBar(float(p.percent / 100), {-1, 26});
            ImGui::Text("%d / %d", p.framesDone, p.framesTotal);
        }
        if (!source.history.empty())
            value(tr(jobs::toString(source.history.back().state)));
        for (const auto& t : source.connection.transfers)
            if (t.state == transfer::TransferState::Transferring)
                ImGui::ProgressBar(t.totalBytes ? float(t.completedBytes) / float(t.totalBytes) : 0,
                                   {-1, 16});
        if (button("Cancel participation", "render.cancel." + std::to_string(source.index)))
            m_controller.cancelParticipation(source.index);
    }
    heading("History");
    if (ImGui::BeginTable("history", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          {0, 260})) {
        for (const auto* key : {"Project", "Chunk", "State"})
            ImGui::TableSetupColumn(tr(key));
        ImGui::TableHeadersRow();
        for (auto it = m_state.clientHistory.rbegin(); it != m_state.clientHistory.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            value(m_projectNames.contains(it->jobId) ? m_projectNames.at(it->jobId)
                                                     : it->jobId.substr(0, 12));
            ImGui::TableNextColumn();
            value(chunkName(it->chunkId));
            ImGui::TableNextColumn();
            value(tr(jobs::toString(it->state)));
        }
        ImGui::EndTable();
    }
}
void Views::logs() {
    heading("Logs");
    if (ImGui::BeginChild("events", {0, 120}, ImGuiChildFlags_Borders)) {
        for (const auto& log : m_state.logs) {
            ImGui::TextDisabled("%s", log.time.c_str());
            ImGui::SameLine();
            value(tr(log.key));
            if (!log.detail.empty()) {
                ImGui::SameLine();
                value(tr(log.detail));
            }
        }
    }
    ImGui::EndChild();
    input("Log path", "logs.path", m_export);
    if (button("Export", "logs.export", !m_state.busy && !m_export.empty()))
        m_controller.exportLogs(pathFromUtf8(m_export));
}
void Views::popups(GLFWwindow* window) {
    const auto id = m_state.offers.empty() ? std::string{}
                                           : std::to_string(m_state.offers.front().source) +
                                                 m_state.offers.front().job.jobId;
    if (id != m_offer) {
        m_offer = id;
        if (!id.empty()) {
            ImGui::OpenPopup("render-offer");
            if (!glfwGetWindowAttrib(window, GLFW_FOCUSED))
                glfwRequestWindowAttention(window);
        }
    }
    ImGui::SetNextWindowSize({480, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("render-offer", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        if (m_state.offers.empty())
            ImGui::CloseCurrentPopup();
        else {
            const auto offer = m_state.offers.front();
            heading("New offer");
            wrapped(offer.job.projectName);
            value(offer.job.engine);
            ImGui::Text("%s: %d - %d / %d", tr("Frames"), offer.job.frameStart, offer.job.frameEnd,
                        offer.job.frameStep);
            ImGui::Text("%s: %s", tr("Estimate"), bytes(offer.job.zipBytes).c_str());
            ImGui::Text("%s: %d s", tr("Time left"),
                        std::max(0, int(std::chrono::duration_cast<std::chrono::seconds>(
                                            offer.deadline - std::chrono::steady_clock::now())
                                            .count())));
            const bool enter = !ImGui::IsWindowAppearing() &&
                               ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                               ImGui::IsKeyPressed(ImGuiKey_Enter, false);
            const bool escape = !ImGui::IsWindowAppearing() &&
                                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            wrapped(tr("Offer shortcuts"));
            if (button("Accept", "offer.accept") || enter) {
                m_controller.decide(offer.source, offer.job.jobId, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (button("Decline", "offer.decline") || escape) {
                m_controller.decide(offer.source, offer.job.jobId, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button("Later", "offer.later")) {
                m_controller.decide(offer.source, offer.job.jobId, false);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (!m_state.settings.benchmarkPrompted && !m_benchmarkDismissed && !m_state.busy &&
        !m_state.settings.blenderVersion.empty() && m_state.offers.empty()) {
        ImGui::OpenPopup("benchmark-prompt");
        m_benchmarkDismissed = true;
    }
    if (ImGui::BeginPopupModal("benchmark-prompt", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        heading("Benchmark prompt");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 450);
        wrapped(tr("Benchmark explanation"));
        ImGui::PopTextWrapPos();
        if (button("Benchmark", "benchmark.accept")) {
            m_controller.benchmark();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Later", "benchmark.later")) {
            auto next = m_edit;
            next.benchmarkPrompted = true;
            m_controller.saveSettings(std::move(next));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} // namespace blendnet::ui
