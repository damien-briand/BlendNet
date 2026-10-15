# Bibliothèque headless : AUCUNE dépendance à Gui/Qml/Quick/Widgets.
# Si un fichier de core inclut un en-tête QML, la compilation échoue (voulu).
add_library(BlendNet-core STATIC
  log/Logger.cpp
  log/Logger.h
  config/AppSettings.cpp
  config/AppSettings.h
)
add_library(BlendNet::core ALIAS BlendNet-core)
target_include_directories(BlendNet-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BlendNet-core PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent)
