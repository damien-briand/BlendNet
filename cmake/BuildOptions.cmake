# RNF-01 / P9: opt-in diagnostics, shared by application code and its tests.
option(BLENDNET_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BLENDNET_BUILD_FUZZER "Build the Clang/libFuzzer protocol harness" OFF)

if(BLENDNET_SANITIZERS)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "BLENDNET_SANITIZERS requires GCC or Clang on a Unix platform")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(BLENDNET_BUILD_FUZZER AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "BLENDNET_BUILD_FUZZER requires Clang with libFuzzer")
endif()
