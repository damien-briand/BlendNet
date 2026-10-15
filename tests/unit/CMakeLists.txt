add_executable(BlendNet-unit-tests
  test_logger.cpp
  test_settings.cpp
)
target_link_libraries(BlendNet-unit-tests PRIVATE BlendNet-core GTest::gtest GTest::gtest_main)
gtest_discover_tests(BlendNet-unit-tests)
