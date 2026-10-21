cmake_minimum_required(VERSION 3.21)

project(BlendNet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLENDNET_BUILD_GUI "Build the desktop application" ON)
option(BLENDNET_TEST_GUI "Register a GUI smoke test (requires a display and OpenGL 3.3)" OFF)

include(FetchContent)
include(CTest)

find_package(Threads REQUIRED)

# RF-10 / RNF-07 : configuration JSON et journalisation.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

find_package(nlohmann_json 3.11 QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(json)
endif()

configure_file(src/core/Version.h.in generated/Version.h @ONLY)

function(blendnet_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Bibliothèque indépendante de toute interface graphique.
add_library(blendnet-core STATIC
    src/core/config/AppSettings.cpp
    src/core/log/Logger.cpp
    src/core/protocol/ProtocolCodec.cpp
)
target_include_directories(blendnet-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src/core"
    "${PROJECT_BINARY_DIR}/generated"
)
target_link_libraries(blendnet-core
    PUBLIC Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json
)
blendnet_warnings(blendnet-core)

# Les rôles seront implémentés dans les phases suivantes.
add_library(blendnet-server STATIC src/server/ServerModule.cpp)
target_link_libraries(blendnet-server PUBLIC blendnet-core)
blendnet_warnings(blendnet-server)

add_library(blendnet-client STATIC src/client/ClientModule.cpp)
target_link_libraries(blendnet-client PUBLIC blendnet-core)
blendnet_warnings(blendnet-client)

# CLI headless : aucune dépendance à ImGui, GLFW ou OpenGL.
add_executable(blendnet-cli tools/blendnet-cli/main.cpp)
target_link_libraries(blendnet-cli PRIVATE blendnet-client)
blendnet_warnings(blendnet-cli)

if(BLENDNET_BUILD_GUI)
    find_package(OpenGL REQUIRED)

    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    # P1 : backend X11 sous Linux ; Wayland pourra être ajouté ensuite.
    if(UNIX AND NOT APPLE)
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()

    find_package(glfw3 3.3 QUIET)
    if(NOT TARGET glfw)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(glfw)
    endif()
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.6
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(blendnet-imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(blendnet-imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(blendnet-imgui PUBLIC
        glfw OpenGL::GL ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(blendnet-imgui PRIVATE
        GL_SILENCE_DEPRECATION
    )

    add_library(blendnet-ui STATIC src/ui/App.cpp)
    target_include_directories(blendnet-ui PUBLIC
        "${PROJECT_SOURCE_DIR}/src/ui"
    )
    target_link_libraries(blendnet-ui
        PUBLIC blendnet-server blendnet-client
        PRIVATE blendnet-imgui
    )
    target_compile_definitions(blendnet-ui PRIVATE
        GL_SILENCE_DEPRECATION
    )
    blendnet_warnings(blendnet-ui)

    add_executable(blendnet src/main.cpp)
    target_link_libraries(blendnet PRIVATE blendnet-ui)
    blendnet_warnings(blendnet)
endif()

if(BUILD_TESTING)
    find_package(GTest 1.14 QUIET)
    if(NOT TARGET GTest::gtest_main)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(blendnet-tests
        tests/unit/AppSettingsTests.cpp
        tests/unit/LoggerTests.cpp
        tests/unit/ProtocolCodecTests.cpp
    )
    target_include_directories(blendnet-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(blendnet-tests PRIVATE
        blendnet-core GTest::gtest_main nlohmann_json::nlohmann_json
    )
    blendnet_warnings(blendnet-tests)

    add_test(NAME blendnet-unit COMMAND blendnet-tests)
    add_test(NAME blendnet-cli-version COMMAND blendnet-cli --version)
    set_tests_properties(blendnet-cli-version PROPERTIES
        PASS_REGULAR_EXPRESSION "BlendNet ${PROJECT_VERSION}"
    )
    add_test(NAME blendnet-cli-help COMMAND blendnet-cli --help)
    add_test(NAME blendnet-cli-invalid-argument COMMAND blendnet-cli --unknown)
    set_tests_properties(blendnet-cli-invalid-argument PROPERTIES WILL_FAIL TRUE)

    if(BLENDNET_BUILD_GUI AND BLENDNET_TEST_GUI)
        add_test(NAME blendnet-gui-smoke COMMAND blendnet --smoke-test
            --config "${PROJECT_BINARY_DIR}/gui-smoke/settings.json"
        )
        set_tests_properties(blendnet-gui-smoke PROPERTIES TIMEOUT 30 LABELS gui)
    endif()
endif()
