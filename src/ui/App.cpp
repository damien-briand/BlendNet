#include "App.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <memory>

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

AppError App::run(int frameLimit) {
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
        glfwCreateWindow(1280, 720, "BlendNet", nullptr, nullptr), glfwDestroyWindow);
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
    if (!ImGui_ImplGlfw_InitForOpenGL(window.get(), true)) {
        return AppError::ImGuiInitFailed;
    }
    imguiLifetime.platformInitialized = true;
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        return AppError::RendererInitFailed;
    }
    imguiLifetime.rendererInitialized = true;

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
        ImGui::NewFrame();
        // P1 : fenêtre vide. Les vues et les snapshots d'état arrivent en P8.
        ImGui::SetNextWindowSize(ImVec2(480.0F, 320.0F), ImGuiCond_FirstUseEver);
        ImGui::Begin("BlendNet");
        ImGui::End();
        ImGui::Render();

        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window.get());
        ++frames;
    }
    return AppError::None;
}

} // namespace blendnet
