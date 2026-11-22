add_library(dsl
  version.cpp)
add_library(dsl::dsl ALIAS dsl)

target_include_directories(dsl PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(dsl PUBLIC cxx_std_20)
target_link_libraries(dsl PRIVATE dsl_warnings)
