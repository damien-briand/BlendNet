# Compile the codec itself with coverage; linking only an instrumented harness
# against an uninstrumented core would leave the parser invisible to libFuzzer.
add_executable(blendnet-protocol-fuzz ProtocolFuzz.cpp
    "${PROJECT_SOURCE_DIR}/src/core/protocol/ProtocolCodec.cpp")
target_include_directories(blendnet-protocol-fuzz PRIVATE "${PROJECT_SOURCE_DIR}/src/core")
target_link_libraries(blendnet-protocol-fuzz PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(blendnet-protocol-fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
target_link_options(blendnet-protocol-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
blendnet_warnings(blendnet-protocol-fuzz)

if(BUILD_TESTING)
    add_test(NAME blendnet-protocol-fuzz-smoke COMMAND blendnet-protocol-fuzz
        -runs=2000 -max_len=65548 -timeout=5)
    set_tests_properties(blendnet-protocol-fuzz-smoke PROPERTIES TIMEOUT 30 LABELS fuzz)
endif()
