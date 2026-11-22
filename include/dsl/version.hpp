#pragma once

#include <string_view>

namespace dsl {

inline constexpr std::string_view kVersion = "0.1.0";

// Версия библиотеки DSL (совпадает с версией проекта в CMake).
std::string_view version() noexcept;

} // namespace dsl
