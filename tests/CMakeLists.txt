include(GoogleTest)

add_executable(dsl_tests
  version_test.cpp
  build/flags_test.cpp)
target_link_libraries(dsl_tests PRIVATE
  dsl::dsl
  GTest::gtest_main
  rapidcheck_gtest
  dsl_warnings)

gtest_discover_tests(dsl_tests)
