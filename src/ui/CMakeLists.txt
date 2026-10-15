# Couche présentation : QML uniquement, aucune logique métier.
qt_add_library(BlendNet-ui STATIC)
qt_add_qml_module(BlendNet-ui
  URI BlendNet.Ui
  VERSION 1.0
  QML_FILES qml/Main.qml
  SOURCES controllers/AppController.cpp controllers/AppController.h
)
target_include_directories(BlendNet-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controllers)
target_compile_definitions(BlendNet-ui PRIVATE BLENDNET_VERSION="${PROJECT_VERSION}")
target_link_libraries(BlendNet-ui PUBLIC BlendNet-server BlendNet-client Qt6::Quick Qt6::Qml)
