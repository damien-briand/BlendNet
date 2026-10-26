#include "App.h"
#include "Views.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

namespace blendnet {
namespace {

struct GlfwLifetime {
    ~GlfwLifetime() { glfwTerminate(); }
};

struct ImGuiLifetime {
    bool platformInitialized = false;
    bool rendererInitialized = false;

    ~ImGuiLifetime() {
        if (rendererInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        if (platformInitialized) {
            ImGui_ImplGlfw_Shutdown();
        }
        ImGui::DestroyContext();
    }
};

} // namespace

AppError App::run(const AppSettings& settings, const std::filesystem::path& config, Logger& logger,
                  int frameLimit, const std::filesystem::path& automation,
                  const std::filesystem::path& screenshot) {
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
    });
    if (!glfwInit()) {
        return AppError::GlfwInitFailed;
    }
    const GlfwLifetime glfwLifetime;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(1380, 900, "BlendNet", nullptr, nullptr), glfwDestroyWindow);
    if (!window) {
        return AppError::WindowCreationFailed;
    }
    glfwMakeContextCurrent(window.get());
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        return AppError::ImGuiInitFailed;
    }
    ImGuiLifetime imguiLifetime;
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.ChildRounding = 8;
    style.FrameRounding = 5;
    style.WindowPadding = {16, 16};
    style.FramePadding = {10, 7};
    style.ItemSpacing = {10, 9};
    style.Colors[ImGuiCol_WindowBg] = {.055F, .075F, .10F, 1};
    style.Colors[ImGuiCol_ChildBg] = {.075F, .10F, .135F, 1};
    style.Colors[ImGuiCol_Button] = {.12F, .25F, .28F, 1};
    style.Colors[ImGuiCol_ButtonHovered] = {.16F, .40F, .40F, 1};
    style.Colors[ImGuiCol_ButtonActive] = {.17F, .50F, .45F, 1};
    style.Colors[ImGuiCol_FrameBg] = {.12F, .16F, .21F, 1};
    style.Colors[ImGuiCol_PlotHistogram] = {.28F, .76F, .65F, 1};
    style.Colors[ImGuiCol_Header] = {.12F, .34F, .35F, 1};
    for (const auto* font :
         {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/segoeui.ttf",
          "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"})
        if (std::filesystem::exists(font)) {
            io.Fonts->AddFontFromFileTTF(font, 17.F);
            break;
        }
    if (!ImGui_ImplGlfw_InitForOpenGL(window.get(), true)) {
        return AppError::ImGuiInitFailed;
    }
    imguiLifetime.platformInitialized = true;
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        return AppError::RendererInitFailed;
    }
    imguiLifetime.rendererInitialized = true;
    application::DesktopController controller(settings, config, logger);
    ui::Automation driver(automation);
    ui::Views views(controller, driver);

    int frames = 0;
    while (!glfwWindowShouldClose(window.get()) && (frameLimit <= 0 || frames < frameLimit)) {
        glfwPollEvents();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window.get(), &width, &height);
        if (width == 0 || height == 0) {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        driver.beforeFrame();
        ImGui::NewFrame();
        views.draw(window.get());
        if (ImGui::IsAnyItemActive() &&
            (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(0) ||
             io.InputQueueCharacters.Size > 0))
            controller.userActivity();
        ImGui::Render();

        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const bool finished = (frameLimit > 0 && frames + 1 >= frameLimit) || driver.done();
        if ((finished || (driver.enabled() && frames % 120 == 0)) && !screenshot.empty()) {
            if (!screenshot.parent_path().empty())
                std::filesystem::create_directories(screenshot.parent_path());
            std::vector<unsigned char> pixels(std::size_t(width) * height * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream file(screenshot, std::ios::binary);
            file << "P6\n" << width << ' ' << height << "\n255\n";
            for (int row = height - 1; row >= 0; --row)
                file.write(
                    reinterpret_cast<const char*>(pixels.data() + std::size_t(row) * width * 3),
                    width * 3);
        }
        glfwSwapBuffers(window.get());
        ++frames;
        if (driver.failed())
            return AppError::AutomationFailed;
        if (driver.done())
            break;
    }
    return AppError::None;
}

} // namespace blendnet
