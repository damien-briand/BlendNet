#pragma once
#include "Automation.h"
#include "application/DesktopController.h"
#include <map>
struct GLFWwindow;
namespace blendnet::ui {
class Views {
  public:
    Views(application::DesktopController& controller, Automation& automation);
    void draw(GLFWwindow* window);

  private:
    application::DesktopController& m_controller;
    Automation& m_test;
    application::DesktopSnapshot m_state;
    AppSettings m_edit;
    std::uint64_t m_revision = 0;
    int m_page = 0, m_settingsTab = 0, m_source = 0, m_job = 0;
    bool m_client = false, m_reveal = false, m_benchmarkDismissed = false;
    std::string m_folder, m_output, m_export, m_selected, m_offer, m_prepared, m_extra;
    jobs::Job m_draft;
    std::map<std::string, std::chrono::steady_clock::time_point> m_started;
    std::map<std::string, std::chrono::steady_clock::time_point> m_finished;
    std::map<std::string, std::string> m_projectNames, m_clientNames;
    const char* tr(std::string_view key) const;
    bool button(std::string_view key, const std::string& id, bool enabled = true,
                ImVec2 size = {0, 0});
    bool input(std::string_view key, const std::string& id, std::string& text,
               ImGuiInputTextFlags flags = 0);
    void pathInput(std::string_view key, const std::string& id, std::filesystem::path& path);
    void heading(std::string_view title);
    void library();
    void project();
    void progress();
    void clients();
    void settings();
    void clientJobs();
    void popups(GLFWwindow* window);
    void logs();
};
} // namespace blendnet::ui
