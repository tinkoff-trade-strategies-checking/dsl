cmake_minimum_required(VERSION 3.25)

project(dsl
  VERSION 0.1.0
  DESCRIPTION "Библиотека языка стратегий DSL"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DSL_BUILD_TESTS "Собирать тесты (нужны GoogleTest и RapidCheck)" ON)

# Детерминизм (INV-4): результат расчёта не зависит от процессора и уровня оптимизации.
# -ffast-math, -Ofast и -funsafe-math-optimizations запрещены в любых флагах сборки.
foreach(flags_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
  if("${${flags_var}}" MATCHES "-ffast-math|-Ofast|-funsafe-math-optimizations|/fp:fast")
    message(FATAL_ERROR "${flags_var} содержит флаг быстрой математики: он нарушает детерминизм (INV-4)")
  endif()
endforeach()

add_library(dsl_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # -ffp-contract=off: без слияния a * b + c в FMA (на x86-64 GCC делает это при -march с FMA).
  target_compile_options(dsl_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off)
elseif(MSVC)
  target_compile_options(dsl_warnings INTERFACE /W4 /WX)
endif()

add_subdirectory(src)

if(DSL_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  find_package(rapidcheck CONFIG REQUIRED)
  add_subdirectory(tests)
endif()
