#pragma once

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace dsl {

inline constexpr std::size_t kMaxSourceBytes = 65536;
inline constexpr std::size_t kMaxRules = 100;
inline constexpr std::size_t kMaxParams = 50;
inline constexpr std::size_t kMaxExprDepth = 100;
inline constexpr std::size_t kMaxAstNodes = 2000;
inline constexpr std::size_t kMaxDagNodes = 256;
inline constexpr std::size_t kMaxLookback = 5000;
inline constexpr std::size_t kMaxSyntaxErrors = 20;
inline constexpr std::int64_t kMaxOrderLots = 1'000'000'000'000LL;
inline constexpr double kMaxDecimalPrice = 1e15;
inline constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

inline bool isNaValue(double Value) noexcept { return std::isnan(Value); }

struct SourcePos {
  std::size_t Line = 0;
  std::size_t Column = 0;
  bool operator==(const SourcePos &) const = default;
  auto operator<=>(const SourcePos &) const = default;
};

struct SourceRange {
  SourcePos Start;
  SourcePos End;
  bool operator==(const SourceRange &) const = default;
  auto operator<=>(const SourceRange &) const = default;
};

struct TextEdit {
  SourceRange Range;
  std::string Replacement;
  bool operator==(const TextEdit &) const = default;
};

struct DiagnosticFix {
  std::string Description;
  std::vector<TextEdit> Edits;
  bool operator==(const DiagnosticFix &) const = default;
};

enum class DiagnosticSeverity { Error, Warning };

struct Diagnostic {
  std::string Code;
  DiagnosticSeverity Severity = DiagnosticSeverity::Error;
  SourceRange Range;
  std::string Message;
  std::vector<DiagnosticFix> Fixes;
  bool operator==(const Diagnostic &) const = default;
};

enum class LookaheadLevel { Ok, Warning, Error };

struct ParamDecl {
  std::string Name;
  double DefaultValue = 0.0;
  std::optional<double> RangeMin;
  std::optional<double> RangeMax;
  std::optional<std::string> StepText;
  bool IsInteger = false;
  bool operator==(const ParamDecl &) const = default;
};

struct CheckResult {
  std::vector<Diagnostic> Diagnostics;
  LookaheadLevel Lookahead = LookaheadLevel::Ok;
  std::vector<ParamDecl> Params;
};

struct Bar {
  std::int64_t Step = 0;
  double Open = 0.0;
  double High = 0.0;
  double Low = 0.0;
  double Close = 0.0;
  double Volume = 0.0;
};

struct PortfolioSnapshot {
  std::int64_t Position = 0;
  double EntryPrice = kNa;
  double Cash = 0.0;
  double Equity = 0.0;
  std::optional<std::int64_t> BarsInPosition;
  std::int64_t LotSize = 1;
  bool PositionReopenedOnBar = false;
};

enum class ActionKind { Buy, Sell, Cancel };
enum class OrderType { Market, Limit, Stop, TakeProfit };
enum class TimeInForce { Gtc, Day };

struct RuleIntent {
  std::size_t RuleId = 0;
  std::string RuleName;
  ActionKind Kind = ActionKind::Buy;
  OrderType OrderKind = OrderType::Market;
  std::int64_t Lots = 0;
  double RawPrice = kNa;
  TimeInForce Tif = TimeInForce::Gtc;
  std::string Because;
  bool operator==(const RuleIntent &Other) const noexcept {
    const bool PricesEqual =
        (isNaValue(RawPrice) && isNaValue(Other.RawPrice)) ||
        RawPrice == Other.RawPrice;
    return RuleId == Other.RuleId && RuleName == Other.RuleName &&
           Kind == Other.Kind && OrderKind == Other.OrderKind &&
           Lots == Other.Lots && PricesEqual && Tif == Other.Tif &&
           Because == Other.Because;
  }
};

struct IntegrityError {
  std::string Code;
  std::string Message;
  bool operator==(const IntegrityError &) const = default;
};

struct StepOutcome {
  std::vector<RuleIntent> Intents;
  std::optional<IntegrityError> Error;
};

} // namespace dsl
