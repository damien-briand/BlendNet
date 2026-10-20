#pragma once

namespace blendnet {

enum class AppError {
    None,
    GlfwInitFailed,
    WindowCreationFailed,
    ImGuiInitFailed,
    RendererInitFailed
};

class App {
  public:
    // Une limite positive permet de vérifier le démarrage et l'arrêt propre de la GUI.
    AppError run(int frameLimit = 0);
};

} // namespace blendnet
