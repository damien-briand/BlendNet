#pragma once
#include "config/AppSettings.h"
#include "log/Logger.h"

namespace blendnet {

enum class AppError {
    None,
    GlfwInitFailed,
    WindowCreationFailed,
    ImGuiInitFailed,
    RendererInitFailed,
    AutomationFailed
};

class App {
  public:
    // Une limite positive permet de vérifier le démarrage et l'arrêt propre de la GUI.
    AppError run(const AppSettings& settings, const std::filesystem::path& config, Logger& logger,
                 int frameLimit = 0, const std::filesystem::path& automation = {},
                 const std::filesystem::path& screenshot = {});
};

} // namespace blendnet
