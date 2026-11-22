#include "internal/format_because.hpp"

#include "dsl/interpreter.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace dsl::internal {

namespace {

constexpr std::size_t kFracDigits = 2;
constexpr std::size_t kToCharsBufferSize = 64;
constexpr int kMinRelevantExp = -3;
constexpr std::chars_format kSciFormat = std::chars_format::scientific;

struct ScientificDecimal {
  std::string Digits;
  int Exp = 0;
};

bool parseScientific(std::string_view Text, ScientificDecimal &Out) {
  const std::size_t ExpPos = Text.find_first_of("eE");
  if (ExpPos == std::string_view::npos || ExpPos == 0 ||
      ExpPos + 1 >= Text.size()) {
    return false;
  }

  Out.Digits.clear();
  Out.Digits.reserve(ExpPos);
  for (std::size_t Idx = 0; Idx < ExpPos; ++Idx) {
    const char Ch = Text[Idx];
    if (Ch == '.') {
      continue;
    }
    if (Ch < '0' || Ch > '9') {
      return false;
    }
    Out.Digits.push_back(Ch);
  }
  if (Out.Digits.empty()) {
    return false;
  }

  std::string_view ExpPart = Text.substr(ExpPos + 1);
  if (ExpPart.front() == '+') {
    ExpPart.remove_prefix(1);
  }
  if (ExpPart.empty()) {
    return false;
  }

  int ParsedExp = 0;
  const char *Begin = ExpPart.data();
  const char *End = Begin + ExpPart.size();
  const std::from_chars_result Res = std::from_chars(Begin, End, ParsedExp);
  if (Res.ec != std::errc{} || Res.ptr != End) {
    return false;
  }

  Out.Exp = ParsedExp;
  return true;
}

char digitAtPower(const ScientificDecimal &Dec, int Power) {
  const int Idx = Dec.Exp - Power;
  if (Idx < 0 || static_cast<std::size_t>(Idx) >= Dec.Digits.size()) {
    return '0';
  }
  return Dec.Digits[static_cast<std::size_t>(Idx)];
}

void incrementDecimal(std::string &Scaled, std::size_t &IntCount) {
  for (std::size_t Idx = Scaled.size(); Idx > 0; --Idx) {
    char &Digit = Scaled[Idx - 1];
    if (Digit < '9') {
      ++Digit;
      return;
    }
    Digit = '0';
  }
  Scaled.insert(Scaled.begin(), '1');
  ++IntCount;
}

} // namespace

std::string formatNumberHalfUp2(double Value) {
  if (!std::isfinite(Value)) {
    return "н/д";
  }
  if (Value == 0.0) {
    return "0";
  }

  const bool IsNegative = Value < 0.0;
  const double AbsValue = std::fabs(Value);

  char Buffer[kToCharsBufferSize]{};
  char *const BufEnd = Buffer + kToCharsBufferSize;
  const auto Conv = std::to_chars(Buffer, BufEnd, AbsValue, kSciFormat);
  if (Conv.ec != std::errc{}) {
    return "н/д";
  }

  ScientificDecimal Dec;
  const auto Len = static_cast<std::size_t>(Conv.ptr - Buffer);
  const std::string_view Raw(Buffer, Len);
  if (!parseScientific(Raw, Dec)) {
    return "н/д";
  }

  if (Dec.Exp < kMinRelevantExp) {
    return "0";
  }

  std::size_t IntCount =
      Dec.Exp >= 0 ? static_cast<std::size_t>(Dec.Exp) + 1 : 1;
  std::string Scaled;
  Scaled.reserve(IntCount + kFracDigits + 1);

  for (std::size_t Idx = 0; Idx < IntCount; ++Idx) {
    const int Power = static_cast<int>(IntCount - 1 - Idx);
    Scaled.push_back(digitAtPower(Dec, Power));
  }
  Scaled.push_back(digitAtPower(Dec, -1));
  Scaled.push_back(digitAtPower(Dec, -2));

  if (digitAtPower(Dec, -3) >= '5') {
    incrementDecimal(Scaled, IntCount);
  }

  if (Scaled.find_first_not_of('0') == std::string::npos) {
    return "0";
  }

  std::size_t FracLen = kFracDigits;
  while (FracLen > 0 && Scaled[IntCount + FracLen - 1] == '0') {
    --FracLen;
  }

  std::string Result;
  Result.reserve((IsNegative ? 1 : 0) + IntCount +
                 (FracLen > 0 ? 1 + FracLen : 0));
  if (IsNegative) {
    Result.push_back('-');
  }
  Result.append(Scaled, 0, IntCount);
  if (FracLen > 0) {
    Result.push_back(',');
    Result.append(Scaled, IntCount, FracLen);
  }
  return Result;
}

std::string formatBoolRu(bool Value) { return Value ? "да" : "нет"; }

} // namespace dsl::internal

namespace dsl {

std::string formatBecauseNumber(double Value) {
  return internal::formatNumberHalfUp2(Value);
}

} // namespace dsl
