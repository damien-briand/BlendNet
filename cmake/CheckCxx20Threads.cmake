# RNF-01: selecting C++20 does not guarantee that the standard library exposes
# jthread. Xcode 16.x still places it in libc++'s experimental library.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

set(_blendnet_thread_probe [=[
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

int main() {
    std::jthread worker([](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(1), [] { return false; });
    });
    worker.request_stop();
}
]=])

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles("${_blendnet_thread_probe}" BLENDNET_HAS_CXX20_THREADS)
if(NOT BLENDNET_HAS_CXX20_THREADS AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_REQUIRED_FLAGS "-fexperimental-library")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fexperimental-library")
    check_cxx_source_compiles("${_blendnet_thread_probe}"
        BLENDNET_HAS_EXPERIMENTAL_CXX20_THREADS)
endif()
cmake_pop_check_state()

if(BLENDNET_HAS_CXX20_THREADS)
    message(STATUS "C++20 jthread, stop_token and interruptible waits are available")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND BLENDNET_HAS_EXPERIMENTAL_CXX20_THREADS)
    # The driver must receive this flag both when compiling C++ and when linking:
    # defining a libc++ implementation macro alone can leave unresolved symbols.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
    add_link_options("$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
    message(STATUS "Enabling -fexperimental-library for Xcode's C++20 thread support")
else()
    message(FATAL_ERROR
        "BlendNet requires std::jthread, std::stop_token and stop-aware condition-variable waits. "
        "The selected compiler/standard library cannot compile and link them. "
        "On macOS, install a complete Xcode toolchain with libc++experimental, or use Xcode 26 or later. "
        "See CMakeFiles/CMakeConfigureLog.yaml (or CMakeError.log) for the feature-check diagnostics.")
endif()

unset(_blendnet_thread_probe)
