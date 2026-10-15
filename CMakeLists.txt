cmake_minimum_required(VERSION 3.21)
project(BlendNet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLENDNET_BUILD_TESTS "Build unit/integration tests" ON)
option(BLENDNET_BUILD_UI "Build the QML UI and main executable" ON)

# Qt Widgets est interdit (spec §2) : on ne le cherche volontairement pas.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Concurrent)
if(BLENDNET_BUILD_UI)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick)
endif()
qt_standard_project_setup()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Ordre = sens des dépendances : core -> server/client -> ui -> exe
add_subdirectory(src/core)
add_subdirectory(src/server)
add_subdirectory(src/client)
add_subdirectory(tools)
if(BLENDNET_BUILD_UI)
  add_subdirectory(src/ui)
  add_subdirectory(src)   # exécutable BlendNet
endif()

if(BLENDNET_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()
