cmake_minimum_required(VERSION 3.21)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif()

project(BlendNet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLENDNET_BUILD_GUI "Build the desktop application" ON)
option(BLENDNET_TEST_GUI "Register a GUI smoke test (requires a display and OpenGL 3.3)" OFF)
option(BLENDNET_TEST_LARGE_TRANSFERS "Register the 1 GiB ZIP transfer/resume test" OFF)
option(BLENDNET_TEST_BLENDER "Register the real Blender CPU render/benchmark test (downloads Blender if absent)" OFF)
set(BLENDNET_BLENDER_TEST_VERSION "4.2.1" CACHE STRING "Official Blender version used by the optional integration test")
set(BLENDNET_BLENDER_TEST_CACHE "${PROJECT_SOURCE_DIR}/.cache/blender" CACHE PATH "Verified Blender cache for the optional integration test")

include(FetchContent)
include(CTest)
include(cmake/BuildOptions.cmake)

find_package(Threads REQUIRED)
include(cmake/CheckCxx20Threads.cmake)
find_package(OpenSSL 1.1.1 REQUIRED)
find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
find_package(ZLIB REQUIRED) # Dépendance de libzip, CRC des images PNG.
find_package(CURL 7.85 REQUIRED)

find_package(reproc 14.2 CONFIG QUIET)
if(NOT TARGET reproc)
    set(REPROC++ OFF CACHE BOOL "" FORCE)
    set(REPROC_TEST OFF CACHE BOOL "" FORCE)
    set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(reproc
        GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
        GIT_TAG 3179928ae7b085e41dfb846d987519fa7c12ffb3 # v14.2.5
    )
    FetchContent_MakeAvailable(reproc)
endif()

# RF-43/53 : archives de résultats, dépendance de la stack contractuelle.
find_package(libzip 1.7 CONFIG QUIET)
if(NOT TARGET libzip::zip)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)
    set(BUILD_OSSFUZZ OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
    set(ENABLE_GNUTLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG 6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea # v1.11.4
    )
    FetchContent_MakeAvailable(libzip)
endif()

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
    src/core/config/RuntimePaths.cpp
    src/core/log/Logger.cpp
    src/core/log/LogExporter.cpp
    src/core/protocol/ProtocolCodec.cpp
    src/core/network/NetworkTypes.cpp
    src/core/network/Security.cpp
    src/core/network/TlsChannel.cpp
    src/core/network/TlsServer.cpp
    src/core/network/ClientSocket.cpp
    src/core/network/TransferLink.cpp
    src/core/transfer/TransferEngine.cpp
    src/core/jobs/Job.cpp
    src/core/jobs/EligibilityFilter.cpp
    src/core/jobs/JobScheduler.cpp
    src/core/jobs/JobOrchestrator.cpp
    src/core/jobs/ResultAssembler.cpp
    src/core/jobs/SimulatedWorker.cpp
    src/core/jobs/JobExecutor.cpp
    src/core/archive/ZipService.cpp
    src/core/jobs/JobFile.cpp
    src/core/projects/ProjectLibrary.cpp
    src/core/application/DesktopController.cpp
    src/core/blender/BlenderTypes.cpp
    src/core/blender/Process.cpp
    src/core/blender/BlenderInstaller.cpp
    src/core/blender/HttpDownload.cpp
    src/core/blender/GpuInfoService.cpp
    src/core/blender/RenderProcess.cpp
    src/core/blender/Benchmark.cpp
)
target_include_directories(blendnet-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src/core"
    "${PROJECT_BINARY_DIR}/generated"
)
target_link_libraries(blendnet-core
    PUBLIC Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto libzip::zip ZLIB::ZLIB
        CURL::libcurl reproc ${CMAKE_DL_LIBS}
)
target_include_directories(blendnet-core SYSTEM PRIVATE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(blendnet-core PRIVATE ASIO_STANDALONE)
if(WIN32)
    target_link_libraries(blendnet-core PRIVATE ws2_32 mswsock)
    target_compile_definitions(blendnet-core PRIVATE _WIN32_WINNT=0x0A00)
endif()

blendnet_warnings(blendnet-core)
add_library(blendnet-i18n STATIC src/ui/I18n.cpp)
target_include_directories(blendnet-i18n PUBLIC "${PROJECT_SOURCE_DIR}/src/ui")
blendnet_warnings(blendnet-i18n)

# Façades des rôles ; la logique réseau headless reste dans core/.
add_library(blendnet-server STATIC src/server/RenderServer.cpp)
target_include_directories(blendnet-server PUBLIC "${PROJECT_SOURCE_DIR}/src/server")
target_link_libraries(blendnet-server PUBLIC blendnet-core)
blendnet_warnings(blendnet-server)

add_library(blendnet-client STATIC src/client/SourceConnection.cpp)
target_include_directories(blendnet-client PUBLIC "${PROJECT_SOURCE_DIR}/src/client")
target_link_libraries(blendnet-client PUBLIC blendnet-core)
blendnet_warnings(blendnet-client)

# CLI headless : aucune dépendance à ImGui, GLFW ou OpenGL.
add_executable(blendnet-cli tools/blendnet-cli/main.cpp)
target_link_libraries(blendnet-cli PRIVATE blendnet-client)
blendnet_warnings(blendnet-cli)

add_executable(blendnet-server-cli tools/blendnet-server/main.cpp)
target_link_libraries(blendnet-server-cli PRIVATE blendnet-server)
blendnet_warnings(blendnet-server-cli)

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
        "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    )
    target_include_directories(blendnet-imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
        "${imgui_SOURCE_DIR}/misc/cpp"
    )
    target_link_libraries(blendnet-imgui PUBLIC
        glfw OpenGL::GL ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(blendnet-imgui PRIVATE
        GL_SILENCE_DEPRECATION
    )

    add_library(blendnet-ui STATIC src/ui/App.cpp src/ui/Views.cpp src/ui/Automation.cpp)
    target_include_directories(blendnet-ui PUBLIC
        "${PROJECT_SOURCE_DIR}/src/ui"
    )
    target_link_libraries(blendnet-ui
        PUBLIC blendnet-server blendnet-client
        PRIVATE blendnet-imgui blendnet-i18n nlohmann_json::nlohmann_json
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
        tests/unit/NetworkTests.cpp
        tests/unit/TransferTests.cpp
        tests/unit/JobTests.cpp
        tests/unit/BlenderTests.cpp
        tests/unit/ZipServiceTests.cpp
        tests/unit/DesktopPolicyTests.cpp
    )
    target_include_directories(blendnet-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(blendnet-tests PRIVATE
        blendnet-core blendnet-i18n GTest::gtest_main nlohmann_json::nlohmann_json libzip::zip ZLIB::ZLIB
    )
    blendnet_warnings(blendnet-tests)
    add_executable(blendnet-fake-blender tests/helpers/FakeBlender.cpp)
    blendnet_warnings(blendnet-fake-blender)
    target_compile_definitions(blendnet-tests PRIVATE BLENDNET_FAKE_BLENDER_PATH="$<TARGET_FILE:blendnet-fake-blender>")
    add_dependencies(blendnet-tests blendnet-fake-blender)
    add_executable(blendnet-desktop-tests tests/integration/DesktopTests.cpp)
    target_include_directories(blendnet-desktop-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_compile_definitions(blendnet-desktop-tests PRIVATE BLENDNET_FAKE_BLENDER_PATH="$<TARGET_FILE:blendnet-fake-blender>")
    target_link_libraries(blendnet-desktop-tests PRIVATE blendnet-core GTest::gtest_main libzip::zip)
    add_dependencies(blendnet-desktop-tests blendnet-fake-blender)
    blendnet_warnings(blendnet-desktop-tests)
    add_test(NAME blendnet-desktop COMMAND blendnet-desktop-tests)
    set_tests_properties(blendnet-desktop PROPERTIES TIMEOUT 60)

    add_test(NAME blendnet-unit COMMAND blendnet-tests)
    add_executable(blendnet-network-tests tests/integration/NetworkTests.cpp)
    target_include_directories(blendnet-network-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_include_directories(blendnet-network-tests SYSTEM PRIVATE "${ASIO_INCLUDE_DIR}")
    target_compile_definitions(blendnet-network-tests PRIVATE ASIO_STANDALONE)
    target_link_libraries(blendnet-network-tests PRIVATE blendnet-server blendnet-client
        GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
    blendnet_warnings(blendnet-network-tests)
    add_test(NAME blendnet-network COMMAND blendnet-network-tests)
    set_tests_properties(blendnet-network PROPERTIES TIMEOUT 30)
    add_executable(blendnet-transfer-tests tests/integration/TransferTests.cpp)
    target_include_directories(blendnet-transfer-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_include_directories(blendnet-transfer-tests SYSTEM PRIVATE "${ASIO_INCLUDE_DIR}")
    target_compile_definitions(blendnet-transfer-tests PRIVATE ASIO_STANDALONE)
    target_link_libraries(blendnet-transfer-tests PRIVATE blendnet-server blendnet-client
        GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
    blendnet_warnings(blendnet-transfer-tests)
    add_test(NAME blendnet-transfer COMMAND blendnet-transfer-tests)
    set_tests_properties(blendnet-transfer PROPERTIES TIMEOUT 90)
    add_executable(blendnet-job-tests tests/integration/JobTests.cpp)
    target_include_directories(blendnet-job-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(blendnet-job-tests PRIVATE blendnet-server blendnet-client GTest::gtest_main)
    blendnet_warnings(blendnet-job-tests)
    add_test(NAME blendnet-jobs COMMAND blendnet-job-tests)
    set_tests_properties(blendnet-jobs PROPERTIES TIMEOUT 60)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME blendnet-project-dependencies COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/project_dependencies.py"
            "${PROJECT_SOURCE_DIR}/src/core/projects/ProjectLibrary.cpp")
        add_test(NAME blendnet-cli-network COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/cli_network.py"
            "$<TARGET_FILE:blendnet-server-cli>" "$<TARGET_FILE:blendnet-cli>")
        set_tests_properties(blendnet-cli-network PROPERTIES TIMEOUT 20)
        add_test(NAME blendnet-cli-transfer COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/cli_transfer.py"
            "$<TARGET_FILE:blendnet-server-cli>" "$<TARGET_FILE:blendnet-cli>")
        set_tests_properties(blendnet-cli-transfer PROPERTIES TIMEOUT 100)
        add_test(NAME blendnet-cli-jobs COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/cli_jobs.py"
            "$<TARGET_FILE:blendnet-server-cli>" "$<TARGET_FILE:blendnet-cli>")
        set_tests_properties(blendnet-cli-jobs PROPERTIES TIMEOUT 45)
        if(BLENDNET_TEST_BLENDER)
            add_test(NAME blendnet-blender-real COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/integration/blender_render.py"
                "$<TARGET_FILE:blendnet-cli>" "$<TARGET_FILE:blendnet-server-cli>"
                "${BLENDNET_BLENDER_TEST_VERSION}" "${BLENDNET_BLENDER_TEST_CACHE}")
            set_tests_properties(blendnet-blender-real PROPERTIES TIMEOUT 900 LABELS blender)
        endif()
        if(BLENDNET_TEST_LARGE_TRANSFERS)
            add_test(NAME blendnet-transfer-large COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/integration/cli_transfer.py"
                "$<TARGET_FILE:blendnet-server-cli>" "$<TARGET_FILE:blendnet-cli>" --large)
            set_tests_properties(blendnet-transfer-large PROPERTIES TIMEOUT 120 LABELS large)
        endif()
    endif()
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
        if(BLENDNET_TEST_BLENDER AND Python3_Interpreter_FOUND)
            add_test(NAME blendnet-gui-flow COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/integration/gui_flow.py"
                "$<TARGET_FILE:blendnet>" "$<TARGET_FILE:blendnet-cli>"
                "${BLENDNET_BLENDER_TEST_VERSION}" "${BLENDNET_BLENDER_TEST_CACHE}"
                "${PROJECT_BINARY_DIR}/gui-artifacts")
            set_tests_properties(blendnet-gui-flow PROPERTIES TIMEOUT 900 LABELS "gui;blender")
        endif()
    endif()
endif()

if(BLENDNET_BUILD_FUZZER)
    add_subdirectory(tests/protocol-fuzz)
endif()

include(cmake/Packaging.cmake)
