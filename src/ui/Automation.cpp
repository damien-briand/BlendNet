#include "Automation.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
namespace blendnet::ui {
Automation::Automation(const std::filesystem::path& path) {
    if (path.empty())
        return;
    m_enabled = true;
    if (std::filesystem::file_size(path) > 65536)
        throw std::runtime_error("Invalid automation");
    std::ifstream input(path);
    const auto data = nlohmann::json::parse(input);
    for (const auto& step : data.at("steps"))
        m_steps.push_back({step.at("id"), step.value("action", std::string("click")),
                           step.value("value", std::string{})});
}
void Automation::item(const std::string& id, bool enabled) {
    if (!enabled || !ImGui::IsItemVisible())
        return;
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    m_items[id] = {(a.x + b.x) / 2, (a.y + b.y) / 2};
}
void Automation::marker(const std::string& id, bool available) {
    if (available)
        m_items[id] = {0, 0};
}
void Automation::beforeFrame() {
    if (!m_enabled || done() || m_failed) {
        m_items.clear();
        return;
    }
    if (++m_frames > 36000) {
        m_failed = true;
        return;
    }
    const auto& step = m_steps[m_step];
    auto& io = ImGui::GetIO();
    io.AddFocusEvent(true);
    if (m_phase == 0) {
        const auto found = m_items.find(step.id);
        if (found == m_items.end()) {
            m_items.clear();
            return;
        }
        m_point = found->second;
        if (step.action == "wait") {
            std::cout << "UI_STEP " << step.id << '\n' << std::flush;
            ++m_step;
            m_items.clear();
            return;
        }
    }
    if (step.action == "delay") {
        if (++m_phase >= std::stoi(step.value)) {
            m_phase = 0;
            ++m_step;
        }
    } else if (step.action == "key") {
        const auto key = step.value == "Tab"      ? ImGuiKey_Tab
                         : step.value == "Escape" ? ImGuiKey_Escape
                                                  : ImGuiKey_Enter;
        if (m_phase < 2)
            io.AddKeyEvent(key, m_phase == 0);
        if (++m_phase >= 4) {
            m_phase = 0;
            ++m_step;
        }
    } else {
        // Keep the pointer over the item through release; GLFW may refresh it each frame.
        io.AddMousePosEvent(m_point.x, m_point.y);
        if (m_phase == 0) {
            io.AddMouseButtonEvent(0, true);
        }
        if (m_phase == 1)
            io.AddMouseButtonEvent(0, false);
        if (step.action == "text") {
            if (m_phase == 3) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_A, true);
            }
            if (m_phase == 4) {
                io.AddKeyEvent(ImGuiKey_A, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
            }
            if (m_phase == 5)
                io.AddInputCharactersUTF8(step.value.c_str());
        }
        if (++m_phase >= (step.action == "text" ? 8 : 4)) {
            std::cout << "UI_STEP " << step.id << '\n' << std::flush;
            m_phase = 0;
            ++m_step;
        }
    }
    m_items.clear();
}
} // namespace blendnet::ui
