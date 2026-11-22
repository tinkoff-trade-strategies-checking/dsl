include(GoogleTest)

add_executable(dsl_tests
  build/flags_test.cpp
  check_test.cpp
  property_test.cpp
  version_test.cpp)
target_link_libraries(dsl_tests PRIVATE
  dsl::dsl
  GTest::gtest_main
  rapidcheck_gtest
  dsl_warnings)

gtest_discover_tests(dsl_tests)
