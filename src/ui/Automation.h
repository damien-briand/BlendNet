#pragma once
#include <filesystem>
#include <imgui.h>
#include <map>
#include <string>
#include <vector>
namespace blendnet::ui {
// Test driver: injects ordinary ImGui mouse/keyboard input; never calls controller actions.
class Automation {
  public:
    explicit Automation(const std::filesystem::path& script = {});
    void item(const std::string& id, bool enabled = true);
    void marker(const std::string& id, bool available);
    void beforeFrame();
    bool enabled() const { return m_enabled; }
    bool done() const { return m_enabled && m_step == m_steps.size(); }
    bool failed() const { return m_failed; }

  private:
    struct Step {
        std::string id, action, value;
    };
    std::vector<Step> m_steps;
    std::map<std::string, ImVec2> m_items;
    std::size_t m_step = 0;
    int m_phase = 0, m_frames = 0;
    bool m_enabled = false, m_failed = false;
    ImVec2 m_point;
};
} // namespace blendnet::ui
