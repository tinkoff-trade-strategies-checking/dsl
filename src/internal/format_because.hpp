#pragma once

#include <string>

namespace dsl::internal {

/// Formats `Value` using `std::to_chars` shortest decimal -> exact decimal
/// `HALF_UP` to 2 decimals -> stripped trailing zeros -> `-0` -> `0`, `,`
/// separator, no scientific notation, and `н/д` for `NaN`/`±Inf`.
std::string formatNumberHalfUp2(double Value);

/// Formats a boolean value as `"да"` or `"нет"`.
std::string formatBoolRu(bool Value);

} // namespace dsl::internal
