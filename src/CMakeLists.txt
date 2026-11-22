add_library(dsl
  check.cpp
  checker.cpp
  lexer.cpp
  linter.cpp
  parser.cpp
  printer.cpp
  version.cpp)
add_library(dsl::dsl ALIAS dsl)

target_include_directories(dsl
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dsl PUBLIC cxx_std_20)
target_link_libraries(dsl PRIVATE dsl_warnings)
