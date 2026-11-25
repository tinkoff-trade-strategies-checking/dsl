#include "internal/analysis.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dsl::internal {
namespace {

struct BuiltinFuncSpec {
  std::size_t Arity = 0;
  std::uint8_t PeriodArgMask = 0;
  std::uint8_t BoolArgMask = 0;
  ValueType ReturnType = ValueType::Number;
  bool IsDagNode = false;
  bool AllowsOffset = false;

  bool isPeriodArg(std::size_t Index) const noexcept {
    return Index < 8U && ((PeriodArgMask & (1U << Index)) != 0U);
  }

  bool isBoolArg(std::size_t Index) const noexcept {
    return Index < 8U && ((BoolArgMask & (1U << Index)) != 0U);
  }
};

bool isPriceSeries(std::string_view Name) noexcept {
  return Name == "open" || Name == "high" || Name == "low" || Name == "close";
}

bool isMarketSeries(std::string_view Name) noexcept {
  return isPriceSeries(Name) || Name == "volume";
}

bool isPortfolioVar(std::string_view Name) noexcept {
  return Name == "position" || Name == "entry_price" || Name == "cash" ||
         Name == "equity" || Name == "bars_in_position";
}

std::optional<BuiltinFuncSpec> getBuiltinFuncSpec(std::string_view Name) {
  if (Name == "obv") {
    return BuiltinFuncSpec{0, 0U, 0U, ValueType::Number, true, true};
  }
  if (Name == "is_doji" || Name == "is_hammer" || Name == "is_shooting_star" ||
      Name == "is_bullish_engulfing" || Name == "is_bearish_engulfing" ||
      Name == "is_inside_bar" || Name == "is_pin_bar") {
    return BuiltinFuncSpec{0, 0U, 0U, ValueType::Bool, true, false};
  }
  if (Name == "atr" || Name == "vwap" || Name == "stoch_k" ||
      Name == "stoch_rsi" || Name == "adx" || Name == "plus_di" ||
      Name == "minus_di" || Name == "cci" || Name == "willr" ||
      Name == "bull_power" || Name == "bear_power" || Name == "mfi" ||
      Name == "pivot" || Name == "pivot_r1" || Name == "pivot_r2" ||
      Name == "pivot_s1" || Name == "pivot_s2") {
    return BuiltinFuncSpec{1, 1U << 0U, 0U, ValueType::Number, true, true};
  }
  if (Name == "bars_since") {
    return BuiltinFuncSpec{1, 0U, 1U << 0U, ValueType::Number, true, true};
  }
  if (Name == "sma" || Name == "ema" || Name == "wma" || Name == "rsi" ||
      Name == "stdev" || Name == "highest" || Name == "lowest" ||
      Name == "change" || Name == "bb_mid" || Name == "roc" ||
      Name == "linreg" || Name == "slope") {
    return BuiltinFuncSpec{2, 1U << 1U, 0U, ValueType::Number, true, true};
  }
  if (Name == "rising" || Name == "falling") {
    return BuiltinFuncSpec{2, 1U << 1U, 0U, ValueType::Bool, true, false};
  }
  if (Name == "supertrend" || Name == "keltner_upper" ||
      Name == "keltner_lower") {
    return BuiltinFuncSpec{2, 1U << 0U, 0U, ValueType::Number, true, true};
  }
  if (Name == "stoch_d" || Name == "swing_high" || Name == "swing_low") {
    return BuiltinFuncSpec{2, (1U << 0U) | (1U << 1U), 0U, ValueType::Number,
                           true, true};
  }
  if (Name == "bb_upper" || Name == "bb_lower") {
    return BuiltinFuncSpec{3, 1U << 1U, 0U, ValueType::Number, true, true};
  }
  if (Name == "macd") {
    return BuiltinFuncSpec{3, (1U << 1U) | (1U << 2U), 0U, ValueType::Number,
                           true, true};
  }
  if (Name == "ult_osc") {
    return BuiltinFuncSpec{3, (1U << 0U) | (1U << 1U) | (1U << 2U), 0U,
                           ValueType::Number, true, true};
  }
  if (Name == "macd_signal" || Name == "macd_hist") {
    return BuiltinFuncSpec{4, (1U << 1U) | (1U << 2U) | (1U << 3U), 0U,
                           ValueType::Number, true, true};
  }
  if (Name == "crossover" || Name == "crossunder") {
    return BuiltinFuncSpec{2, 0U, 0U, ValueType::Bool, true, false};
  }
  if (Name == "min" || Name == "max") {
    return BuiltinFuncSpec{2, 0U, 0U, ValueType::Number, false, false};
  }
  if (Name == "abs" || Name == "sqrt") {
    return BuiltinFuncSpec{1, 0U, 0U, ValueType::Number, false, false};
  }
  if (Name == "is_na") {
    return BuiltinFuncSpec{1, 0U, 0U, ValueType::Bool, false, false};
  }
  return std::nullopt;
}

bool isDirectPriceIndicatorName(std::string_view Name) noexcept {
  return Name == "sma" || Name == "ema" || Name == "wma" || Name == "highest" ||
         Name == "lowest" || Name == "bb_mid" || Name == "bb_upper" ||
         Name == "bb_lower" || Name == "linreg";
}

bool isSpaceChar(std::string_view Text, std::size_t Pos,
                 std::size_t &Step) noexcept {
  const auto Byte = static_cast<unsigned char>(Text[Pos]);
  if (Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n') {
    Step = 1;
    return true;
  }
  if (Pos + 1 < Text.size() && Byte == 0xC2U &&
      static_cast<unsigned char>(Text[Pos + 1]) == 0xA0U) {
    Step = 2;
    return true;
  }
  return false;
}

std::string trimWhitespace(std::string_view Text) {
  std::size_t Start = 0;
  while (Start < Text.size()) {
    std::size_t Step = 0;
    if (!isSpaceChar(Text, Start, Step)) {
      break;
    }
    Start += Step;
  }
  std::size_t End = Text.size();
  while (End > Start) {
    if (Text[End - 1] == ' ' || Text[End - 1] == '\t' ||
        Text[End - 1] == '\r' || Text[End - 1] == '\n') {
      --End;
    } else if (End >= Start + 2 &&
               static_cast<unsigned char>(Text[End - 2]) == 0xC2U &&
               static_cast<unsigned char>(Text[End - 1]) == 0xA0U) {
      End -= 2;
    } else {
      break;
    }
  }
  return std::string(Text.substr(Start, End - Start));
}

bool isIntValue(double Val) noexcept {
  return std::isfinite(Val) && std::trunc(Val) == Val;
}

struct ParamMeta {
  const ParamStmt *Stmt = nullptr;
  std::size_t DeclIndex = 0;
  bool DefaultInRange = true;
  bool HasValidOverride = false;
};

class SemanticAnalyzer {
public:
  explicit SemanticAnalyzer(std::vector<Diagnostic> &Diagnostics)
      : OutDiagnostics(Diagnostics) {}

  AnalysisState run(Program &Ast,
                    const std::map<std::string, double> &ParamOverrides) {
    processParams(Ast, ParamOverrides);
    markIntegerParamUses(Ast);
    validateIntegerParams();
    validateRules(Ast);
    if (!DagLimitReported) {
      analyzeLookahead(Ast);
    }
    return std::move(State);
  }

private:
  void emitDiag(std::string Code, DiagnosticSeverity Severity,
                SourceRange Range, std::string Message) {
    if (Code == "E-TYPE" || Code == "E-BEC") {
      State.HasPhase2Errors = true;
    }
    Diagnostic Diag;
    Diag.Code = std::move(Code);
    Diag.Severity = Severity;
    Diag.Range = Range;
    Diag.Message = std::move(Message);
    OutDiagnostics.push_back(std::move(Diag));
  }

  void emitTypeError(SourceRange Range, std::string Message) {
    emitDiag("E-TYPE", DiagnosticSeverity::Error, Range, std::move(Message));
  }

  void escalateLookahead(LookaheadLevel Level) noexcept {
    if (static_cast<int>(Level) > static_cast<int>(State.Lookahead)) {
      State.Lookahead = Level;
    }
  }

  void processParams(const Program &Ast,
                     const std::map<std::string, double> &ParamOverrides) {
    for (const auto &Param : Ast.Params) {
      if (isBuiltinName(Param.Name)) {
        std::string Msg = "Имя параметра '" + Param.Name;
        Msg += "' совпадает со встроенным именем";
        emitTypeError(Param.NameRange, std::move(Msg));
        continue;
      }
      if (State.ParamMap.contains(Param.Name)) {
        std::string Msg = "Повторное объявление параметра '" + Param.Name + "'";
        emitTypeError(Param.NameRange, std::move(Msg));
        continue;
      }

      bool Valid = true;
      bool DefaultInRange = true;
      if (!std::isfinite(Param.DefaultValue)) {
        std::string Msg = "Значение параметра '" + Param.Name;
        Msg += "' должно быть конечным числом";
        emitTypeError(Param.DefaultRange, std::move(Msg));
        Valid = false;
        DefaultInRange = false;
      }
      if (Param.HasRange) {
        bool RangeFinite = true;
        if (!std::isfinite(Param.RangeMin)) {
          emitTypeError(Param.RangeMinRange,
                        "Нижняя граница range должна быть конечной");
          Valid = false;
          RangeFinite = false;
        }
        if (!std::isfinite(Param.RangeMax)) {
          emitTypeError(Param.RangeMaxRange,
                        "Верхняя граница range должна быть конечной");
          Valid = false;
          RangeFinite = false;
        }
        if (RangeFinite && Param.RangeMin > Param.RangeMax) {
          emitTypeError({Param.RangeMinRange.Start, Param.RangeMaxRange.End},
                        "Нижняя граница range больше верхней");
          Valid = false;
        } else if (RangeFinite && std::isfinite(Param.DefaultValue) &&
                   (Param.DefaultValue < Param.RangeMin ||
                    Param.DefaultValue > Param.RangeMax)) {
          emitTypeError(Param.DefaultRange,
                        "Значение по умолчанию параметра '" + Param.Name +
                            "' вне диапазона range");
          Valid = false;
          DefaultInRange = false;
        }
      }
      if (Param.HasStep &&
          (!std::isfinite(Param.StepValue) || Param.StepValue <= 0.0)) {
        emitTypeError(Param.StepRange,
                      "Шаг параметра '" + Param.Name + "' должен быть > 0");
        Valid = false;
      }

      ParamDecl Decl;
      Decl.Name = Param.Name;
      Decl.DefaultValue = Param.DefaultValue;
      if (Param.HasRange) {
        Decl.RangeMin = Param.RangeMin;
        Decl.RangeMax = Param.RangeMax;
      }
      if (Param.HasStep) {
        Decl.StepText = Param.StepRawText;
      }
      Decl.IsInteger = false;

      ResolvedParam Resolved;
      Resolved.Name = Param.Name;
      Resolved.EffectiveValue = Param.DefaultValue;
      Resolved.IsInteger = false;
      Resolved.HasRange = Param.HasRange;
      Resolved.RangeMin = Param.RangeMin;
      Resolved.RangeMax = Param.RangeMax;
      Resolved.Valid = Valid;

      const std::size_t Idx = State.ParamDecls.size();
      State.ParamDecls.push_back(std::move(Decl));
      State.ParamMap.emplace(Param.Name, std::move(Resolved));
      MetaMap.emplace(Param.Name,
                      ParamMeta{&Param, Idx, DefaultInRange, false});
    }

    for (const auto &[Key, Val] : ParamOverrides) {
      auto It = State.ParamMap.find(Key);
      if (It == State.ParamMap.end()) {
        emitTypeError({{0, 0}, {0, 0}},
                      "Неизвестный параметр '" + Key + "' в переопределении");
        continue;
      }
      ResolvedParam &Resolved = It->second;
      ParamMeta &Meta = MetaMap.at(Key);
      if (!std::isfinite(Val)) {
        std::string Msg = "Переопределение параметра '" + Key;
        Msg += "' должно быть конечным числом";
        emitTypeError({{0, 0}, {0, 0}}, std::move(Msg));
        Resolved.Valid = false;
        continue;
      }
      if (Resolved.HasRange && Resolved.RangeMin <= Resolved.RangeMax &&
          (Val < Resolved.RangeMin || Val > Resolved.RangeMax)) {
        std::string Msg = "Переопределение параметра '" + Key;
        Msg += "' выходит за пределы range";
        emitTypeError({{0, 0}, {0, 0}}, std::move(Msg));
        Resolved.Valid = false;
        continue;
      }
      Resolved.EffectiveValue = Val;
      Meta.HasValidOverride = true;
    }
  }

  void setParamInteger(const std::string &Name) {
    auto It = State.ParamMap.find(Name);
    if (It == State.ParamMap.end()) {
      return;
    }
    It->second.IsInteger = true;
    const std::size_t Idx = MetaMap.at(Name).DeclIndex;
    State.ParamDecls[Idx].IsInteger = true;
  }

  bool isIndexableTarget(const Expr &Target) const {
    if (const auto *Id = std::get_if<IdentExpr>(&Target.Node)) {
      return isMarketSeries(Id->Name);
    }
    if (const auto *Call = std::get_if<CallExpr>(&Target.Node)) {
      const auto Spec = getBuiltinFuncSpec(Call->Callee);
      return Spec.has_value() && Spec->AllowsOffset &&
             Call->Args.size() == Spec->Arity;
    }
    return false;
  }

  void markExprIntegerUses(const Expr *Node) {
    if (Node == nullptr) {
      return;
    }
    if (const auto *Offset = std::get_if<OffsetExpr>(&Node->Node)) {
      markExprIntegerUses(Offset->Target.get());
      if (Offset->OffsetIsIdent && Offset->Target &&
          isIndexableTarget(*Offset->Target)) {
        setParamInteger(Offset->OffsetIdent);
      }
      return;
    }
    if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
      markExprIntegerUses(Unary->Operand.get());
      return;
    }
    if (const auto *Binary = std::get_if<BinaryExpr>(&Node->Node)) {
      markExprIntegerUses(Binary->Left.get());
      markExprIntegerUses(Binary->Right.get());
      return;
    }
    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      const auto Spec = getBuiltinFuncSpec(Call->Callee);
      if (Spec.has_value() && Spec->PeriodArgMask != 0U &&
          Call->Args.size() == Spec->Arity) {
        for (std::size_t I = 0; I < Call->Args.size(); ++I) {
          if (!Spec->isPeriodArg(I)) {
            continue;
          }
          const Expr *PeriodArg = Call->Args[I].get();
          if (PeriodArg != nullptr) {
            if (const auto *Id = std::get_if<IdentExpr>(&PeriodArg->Node)) {
              setParamInteger(Id->Name);
            }
          }
        }
      }
      for (const auto &Arg : Call->Args) {
        markExprIntegerUses(Arg.get());
      }
    }
  }

  void markIntegerParamUses(const Program &Ast) {
    for (const auto &Rule : Ast.Rules) {
      markExprIntegerUses(Rule.Condition.get());
      markExprIntegerUses(Rule.Action.SizeExpr.get());
      markExprIntegerUses(Rule.Action.PriceExpr.get());
      if (Rule.Because.Present) {
        for (const auto &Part : Rule.Because.Parts) {
          if (Part.IsExpr) {
            markExprIntegerUses(Part.Expression.get());
          }
        }
      }
    }
  }

  void validateIntegerParams() {
    for (auto &[Name, Resolved] : State.ParamMap) {
      if (!Resolved.IsInteger) {
        continue;
      }
      const ParamMeta &Meta = MetaMap.at(Name);
      const ParamStmt &Stmt = *Meta.Stmt;
      if (Meta.DefaultInRange && !isIntValue(Stmt.DefaultValue)) {
        emitTypeError(Stmt.DefaultRange,
                      "Целочисленный параметр '" + Name +
                          "' должен иметь целое значение по умолчанию");
        Resolved.Valid = false;
      }
      if (Stmt.HasRange) {
        if (std::isfinite(Stmt.RangeMin) && !isIntValue(Stmt.RangeMin)) {
          emitTypeError(Stmt.RangeMinRange,
                        "Нижняя граница range целочисленного параметра '" +
                            Name + "' должна быть целой");
          Resolved.Valid = false;
        }
        if (std::isfinite(Stmt.RangeMax) && !isIntValue(Stmt.RangeMax)) {
          emitTypeError(Stmt.RangeMaxRange,
                        "Верхняя граница range целочисленного параметра '" +
                            Name + "' должна быть целой");
          Resolved.Valid = false;
        }
      }
      if (Stmt.HasStep && std::isfinite(Stmt.StepValue) &&
          Stmt.StepValue > 0.0 && !isIntValue(Stmt.StepValue)) {
        std::string Msg = "Шаг целочисленного параметра '" + Name;
        Msg += "' должен быть целым";
        emitTypeError(Stmt.StepRange, std::move(Msg));
        Resolved.Valid = false;
      }
      if (Meta.HasValidOverride && !isIntValue(Resolved.EffectiveValue)) {
        emitTypeError({{0, 0}, {0, 0}},
                      "Переопределение целочисленного параметра '" + Name +
                          "' должно быть целым числом");
        Resolved.Valid = false;
      }
    }
  }

  void validateRules(Program &Ast) {
    std::set<std::string> SeenRuleNames;
    for (auto &Rule : Ast.Rules) {
      const std::string TrimmedName = trimWhitespace(Rule.Name);
      if (TrimmedName.empty()) {
        emitTypeError(Rule.NameRange, "Имя правила не может быть пустым");
      } else if (!SeenRuleNames.insert(TrimmedName).second) {
        emitTypeError(Rule.NameRange,
                      "Повторяющееся имя правила '" + TrimmedName + "'");
      }

      if (Rule.Condition) {
        const ValueType CondType = checkExpr(*Rule.Condition, false);
        if (CondType != ValueType::Error && CondType != ValueType::Bool) {
          emitTypeError(Rule.Condition->Range,
                        "Условие when должно иметь тип bool");
        }
      }

      validateAction(Rule.Action);

      if (Rule.Because.Present) {
        bool HasExprPart = false;
        std::string CombinedLiteral;
        for (auto &Part : Rule.Because.Parts) {
          if (Part.IsExpr) {
            HasExprPart = true;
            if (Part.Expression) {
              checkExpr(*Part.Expression, false);
            }
          } else {
            CombinedLiteral.append(Part.LiteralText);
          }
        }
        if (!HasExprPart && trimWhitespace(CombinedLiteral).empty()) {
          emitDiag("E-BEC", DiagnosticSeverity::Error, Rule.Because.Range,
                   "Причина because не может быть пустой");
        }
      }
    }
  }

  void validateAction(ActionNode &Action) {
    if (Action.Kind == ActionKind::Cancel) {
      return;
    }
    if (Action.Kind == ActionKind::Buy && Action.Unit == SizeUnit::All) {
      emitTypeError(Action.SizeRange,
                    "Действие 'buy all' запрещено; используйте % cash или "
                    "equity");
    } else if (Action.Kind == ActionKind::Sell &&
               (Action.Unit == SizeUnit::PercentEquity ||
                Action.Unit == SizeUnit::PercentCash)) {
      emitTypeError(Action.SizeRange,
                    "Действие 'sell % equity/cash' запрещено; используйте "
                    "lots, % position или all");
    }
    if (Action.Kind == ActionKind::Buy &&
        Action.OrderKind == OrderType::TakeProfit) {
      emitTypeError(Action.OrderRange,
                    "Тип заявки take_profit допустим только для продажи");
    }
    if (Action.SizeExpr) {
      const ValueType SizeType = checkExpr(*Action.SizeExpr, false);
      if (SizeType != ValueType::Error && SizeType != ValueType::Number) {
        emitTypeError(Action.SizeExpr->Range,
                      "Размер заявки должен иметь числовой тип");
      }
    }
    if (Action.PriceExpr) {
      const ValueType PriceType = checkExpr(*Action.PriceExpr, false);
      if (PriceType != ValueType::Error && PriceType != ValueType::Number) {
        emitTypeError(Action.PriceExpr->Range,
                      "Цена заявки должна иметь числовой тип");
      }
    }
  }

  ValueType checkExpr(Expr &Node, bool InStatefulArg) {
    ValueType Result = ValueType::Error;
    if (std::holds_alternative<NumberLitExpr>(Node.Node)) {
      Result = ValueType::Number;
    } else if (std::holds_alternative<BoolLitExpr>(Node.Node)) {
      Result = ValueType::Bool;
    } else if (const auto *Id = std::get_if<IdentExpr>(&Node.Node)) {
      Result = checkIdentExpr(Node.Range, *Id, InStatefulArg);
    } else if (auto *Offset = std::get_if<OffsetExpr>(&Node.Node)) {
      Result = checkOffsetExpr(Node.Range, *Offset, InStatefulArg);
    } else if (auto *Unary = std::get_if<UnaryExpr>(&Node.Node)) {
      Result = checkUnaryExpr(Node.Range, *Unary, InStatefulArg);
    } else if (auto *Binary = std::get_if<BinaryExpr>(&Node.Node)) {
      Result = checkBinaryExpr(Node.Range, *Binary, InStatefulArg);
    } else if (auto *Call = std::get_if<CallExpr>(&Node.Node)) {
      Result = checkCallExpr(Node.Range, *Call, InStatefulArg);
    }
    Node.ResolvedType = Result;
    return Result;
  }

  ValueType checkIdentExpr(SourceRange Range, const IdentExpr &Id,
                           bool InStatefulArg) {
    if (isMarketSeries(Id.Name)) {
      return ValueType::Number;
    }
    if (isPortfolioVar(Id.Name)) {
      if (InStatefulArg) {
        std::string Msg = "Переменная портфеля '" + Id.Name;
        Msg += "' запрещена внутри индикаторов и межбаровых функций";
        emitTypeError(Range, std::move(Msg));
        return ValueType::Error;
      }
      return ValueType::Number;
    }
    if (const auto It = State.ParamMap.find(Id.Name);
        It != State.ParamMap.end()) {
      return It->second.Valid ? ValueType::Number : ValueType::Error;
    }
    emitTypeError(Range, "Неизвестный идентификатор '" + Id.Name + "'");
    return ValueType::Error;
  }

  ValueType checkOffsetExpr(SourceRange Range, OffsetExpr &Offset,
                            bool InStatefulArg) {
    if (!Offset.Target) {
      return ValueType::Error;
    }
    bool TargetOk = false;
    if (const auto *Id = std::get_if<IdentExpr>(&Offset.Target->Node)) {
      if (isMarketSeries(Id->Name)) {
        Offset.Target->ResolvedType = ValueType::Number;
        TargetOk = true;
      } else if (isPortfolioVar(Id->Name)) {
        std::string Msg = "Индексация [...] для переменной портфеля '";
        Msg += Id->Name + "' запрещена";
        emitTypeError(Range, std::move(Msg));
        Offset.Target->ResolvedType = ValueType::Error;
      } else if (State.ParamMap.contains(Id->Name)) {
        std::string Msg = "Индексация [...] для параметра '" + Id->Name;
        Msg += "' запрещена";
        emitTypeError(Range, std::move(Msg));
        Offset.Target->ResolvedType = ValueType::Error;
      } else {
        std::string Msg = "Неизвестный идентификатор '" + Id->Name + "'";
        emitTypeError(Offset.Target->Range, std::move(Msg));
        Offset.Target->ResolvedType = ValueType::Error;
      }
    } else if (const auto *Call = std::get_if<CallExpr>(&Offset.Target->Node)) {
      const ValueType TargetType = checkExpr(*Offset.Target, InStatefulArg);
      if (TargetType != ValueType::Error) {
        const auto Spec = getBuiltinFuncSpec(Call->Callee);
        if (Spec.has_value() && Spec->AllowsOffset) {
          TargetOk = true;
        } else {
          std::string Msg = "Индексация [...] для функции '" + Call->Callee;
          Msg += "' запрещена";
          emitTypeError(Range, std::move(Msg));
        }
      }
    } else {
      checkExpr(*Offset.Target, InStatefulArg);
      emitTypeError(Range, "Недопустимая цель индексации [...]");
    }
    if (!TargetOk) {
      return ValueType::Error;
    }

    if (!Offset.OffsetIsIdent) {
      const std::int64_t Eff =
          Offset.HasUnaryMinus ? -Offset.OffsetInt : Offset.OffsetInt;
      if (Eff > static_cast<std::int64_t>(kMaxLookback)) {
        emitTypeError(Offset.OffsetRange,
                      "Смещение не должно превышать 5000 баров");
        return ValueType::Error;
      }
      return ValueType::Number;
    }

    const auto It = State.ParamMap.find(Offset.OffsetIdent);
    if (It == State.ParamMap.end()) {
      std::string Msg = "Неизвестный параметр '" + Offset.OffsetIdent;
      Msg += "' в смещении";
      emitTypeError(Offset.OffsetRange, std::move(Msg));
      return ValueType::Error;
    }
    const ResolvedParam &Param = It->second;
    if (!Param.Valid) {
      return ValueType::Error;
    }
    const double Eff =
        Offset.HasUnaryMinus ? -Param.EffectiveValue : Param.EffectiveValue;
    const double MaxBound =
        Param.HasRange
            ? (Offset.HasUnaryMinus ? -Param.RangeMin : Param.RangeMax)
            : Eff;
    if (Eff > static_cast<double>(kMaxLookback) ||
        MaxBound > static_cast<double>(kMaxLookback)) {
      emitTypeError(Offset.OffsetRange,
                    "Смещение не должно превышать 5000 баров");
      return ValueType::Error;
    }
    return ValueType::Number;
  }

  ValueType checkUnaryExpr(SourceRange Range, UnaryExpr &Unary,
                           bool InStatefulArg) {
    if (!Unary.Operand) {
      return ValueType::Error;
    }
    const ValueType OpType = checkExpr(*Unary.Operand, InStatefulArg);
    if (OpType == ValueType::Error) {
      return ValueType::Error;
    }
    if (Unary.Op == UnaryOp::Neg) {
      if (OpType != ValueType::Number) {
        emitTypeError(Range, "Унарный '-' требует числовой операнд");
        return ValueType::Error;
      }
      return ValueType::Number;
    }
    if (OpType != ValueType::Bool) {
      emitTypeError(Range, "Оператор 'not' требует логический операнд");
      return ValueType::Error;
    }
    return ValueType::Bool;
  }

  ValueType checkBinaryExpr(SourceRange Range, BinaryExpr &Binary,
                            bool InStatefulArg) {
    if (!Binary.Left || !Binary.Right) {
      return ValueType::Error;
    }
    const ValueType LeftType = checkExpr(*Binary.Left, InStatefulArg);
    const ValueType RightType = checkExpr(*Binary.Right, InStatefulArg);
    if (LeftType == ValueType::Error || RightType == ValueType::Error) {
      return ValueType::Error;
    }

    switch (Binary.Op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
      if (LeftType != ValueType::Number || RightType != ValueType::Number) {
        emitTypeError(Range, "Арифметический оператор требует числа");
        return ValueType::Error;
      }
      return ValueType::Number;
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      if (LeftType != ValueType::Number || RightType != ValueType::Number) {
        emitTypeError(Range, "Оператор сравнения порядка требует числа");
        return ValueType::Error;
      }
      return ValueType::Bool;
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      if (LeftType != RightType) {
        emitTypeError(Range, "Операторы '==' и '!=' требуют одинаковые типы");
        return ValueType::Error;
      }
      return ValueType::Bool;
    case BinaryOp::And:
    case BinaryOp::Or:
      if (LeftType != ValueType::Bool || RightType != ValueType::Bool) {
        emitTypeError(Range, "Логический оператор требует тип bool");
        return ValueType::Error;
      }
      return ValueType::Bool;
    }
    return ValueType::Error;
  }
  bool checkPeriodArg(Expr &Arg) {
    if (const auto *Lit = std::get_if<NumberLitExpr>(&Arg.Node)) {
      if (!Lit->IsInteger || Lit->Value < 1.0 ||
          Lit->Value > static_cast<double>(kMaxLookback)) {
        emitTypeError(Arg.Range,
                      "Период функции должен быть целым числом от 1 до 5000");
        Arg.ResolvedType = ValueType::Error;
        return false;
      }
      Arg.ResolvedType = ValueType::Number;
      return true;
    }
    if (const auto *Id = std::get_if<IdentExpr>(&Arg.Node)) {
      const auto It = State.ParamMap.find(Id->Name);
      if (It == State.ParamMap.end()) {
        std::string Msg = "Неизвестный параметр '" + Id->Name;
        Msg += "' в позиции периода";
        emitTypeError(Arg.Range, std::move(Msg));
        Arg.ResolvedType = ValueType::Error;
        return false;
      }
      const ResolvedParam &Param = It->second;
      if (!Param.Valid) {
        Arg.ResolvedType = ValueType::Error;
        return false;
      }
      if (Param.EffectiveValue < 1.0 ||
          Param.EffectiveValue > static_cast<double>(kMaxLookback) ||
          (Param.HasRange &&
           (Param.RangeMin < 1.0 ||
            Param.RangeMax > static_cast<double>(kMaxLookback)))) {
        std::string Msg = "Период параметра '" + Id->Name;
        Msg += "' должен лежать в пределах 1..5000";
        emitTypeError(Arg.Range, std::move(Msg));
        Arg.ResolvedType = ValueType::Error;
        return false;
      }
      Arg.ResolvedType = ValueType::Number;
      return true;
    }
    emitTypeError(Arg.Range,
                  "Период функции должен быть целым литералом или параметром");
    Arg.ResolvedType = ValueType::Error;
    return false;
  }

  ValueType checkCallExpr(SourceRange Range, CallExpr &Call,
                          bool InStatefulArg) {
    const auto Spec = getBuiltinFuncSpec(Call.Callee);
    if (!Spec.has_value()) {
      emitTypeError(Call.CalleeRange,
                    "Неизвестная функция '" + Call.Callee + "'");
      for (auto &Arg : Call.Args) {
        if (Arg) {
          checkExpr(*Arg, InStatefulArg);
        }
      }
      return ValueType::Error;
    }
    if (Call.Args.size() != Spec->Arity) {
      std::string Msg = "Неверное число аргументов для функции '";
      Msg += Call.Callee + "'";
      emitTypeError(Range, std::move(Msg));
      const bool ChildStateful = InStatefulArg || Spec->IsDagNode;
      for (auto &Arg : Call.Args) {
        if (Arg) {
          checkExpr(*Arg, ChildStateful);
        }
      }
      return ValueType::Error;
    }

    const bool ChildStateful = InStatefulArg || Spec->IsDagNode;
    bool AllValid = true;
    for (std::size_t I = 0; I < Call.Args.size(); ++I) {
      if (!Call.Args[I]) {
        AllValid = false;
        continue;
      }
      if (Spec->isPeriodArg(I)) {
        if (!checkPeriodArg(*Call.Args[I])) {
          AllValid = false;
        }
        continue;
      }
      const ValueType ArgType = checkExpr(*Call.Args[I], ChildStateful);
      if (ArgType == ValueType::Error) {
        AllValid = false;
      } else if (Spec->isBoolArg(I)) {
        if (ArgType != ValueType::Bool) {
          std::string Msg = "Аргумент функции '" + Call.Callee;
          Msg += "' должен иметь логический тип";
          emitTypeError(Call.Args[I]->Range, std::move(Msg));
          AllValid = false;
        }
      } else if (ArgType != ValueType::Number) {
        std::string Msg = "Аргумент функции '" + Call.Callee;
        Msg += "' должен иметь числовой тип";
        emitTypeError(Call.Args[I]->Range, std::move(Msg));
        AllValid = false;
      }
    }

    if (!AllValid) {
      return ValueType::Error;
    }

    if (Spec->IsDagNode) {
      std::string Key = "C(" + Call.Callee;
      for (const auto &Arg : Call.Args) {
        Key.push_back(',');
        Key.append(canonicalExprKey(*Arg));
      }
      Key.push_back(')');
      if (UniqueDagKeys.insert(std::move(Key)).second) {
        State.UniqueDagNodes = UniqueDagKeys.size();
        if (State.UniqueDagNodes > kMaxDagNodes && !DagLimitReported) {
          DagLimitReported = true;
          emitDiag("E-SYN", DiagnosticSeverity::Error, Range,
                   "Превышено максимальное число уникальных узлов "
                   "индикаторов (256)");
        }
      }
    }
    return Spec->ReturnType;
  }

  static std::string encodeDouble(double Val) {
    const double Norm = (Val == 0.0) ? 0.0 : Val;
    return "N:" + std::to_string(std::bit_cast<std::uint64_t>(Norm));
  }

  std::string canonicalExprKey(const Expr &Node) const {
    return std::visit(
        [this](const auto &Inner) -> std::string {
          using T = std::decay_t<decltype(Inner)>;
          if constexpr (std::is_same_v<T, NumberLitExpr>) {
            return encodeDouble(Inner.Value);
          } else if constexpr (std::is_same_v<T, BoolLitExpr>) {
            return Inner.Value ? "B:1" : "B:0";
          } else if constexpr (std::is_same_v<T, IdentExpr>) {
            const auto It = State.ParamMap.find(Inner.Name);
            if (It != State.ParamMap.end()) {
              return encodeDouble(It->second.EffectiveValue);
            }
            return "I:" + Inner.Name;
          } else if constexpr (std::is_same_v<T, OffsetExpr>) {
            double EffVal = 0.0;
            if (Inner.OffsetIsIdent) {
              const auto It = State.ParamMap.find(Inner.OffsetIdent);
              if (It != State.ParamMap.end()) {
                EffVal = It->second.EffectiveValue;
              }
              if (Inner.HasUnaryMinus) {
                EffVal = -EffVal;
              }
            } else {
              const std::int64_t EffInt =
                  Inner.HasUnaryMinus ? -Inner.OffsetInt : Inner.OffsetInt;
              EffVal = static_cast<double>(EffInt);
            }
            std::string TargetKey;
            if (Inner.Target) {
              TargetKey = canonicalExprKey(*Inner.Target);
            }
            if (EffVal == 0.0) {
              return TargetKey;
            }
            return "O(" + TargetKey + "," + encodeDouble(EffVal) + ")";
          } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            const char *OpKey = Inner.Op == UnaryOp::Neg ? "neg" : "not";
            std::string SubKey;
            if (Inner.Operand) {
              SubKey = canonicalExprKey(*Inner.Operand);
            }
            return std::string("U(") + OpKey + "," + SubKey + ")";
          } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            const std::string LeftKey =
                Inner.Left ? canonicalExprKey(*Inner.Left) : std::string{};
            const std::string RightKey =
                Inner.Right ? canonicalExprKey(*Inner.Right) : std::string{};
            return "B(" + std::to_string(static_cast<int>(Inner.Op)) + "," +
                   LeftKey + "," + RightKey + ")";
          } else {
            std::string Key = "C(" + Inner.Callee;
            for (const auto &Arg : Inner.Args) {
              Key.push_back(',');
              if (Arg) {
                Key.append(canonicalExprKey(*Arg));
              }
            }
            Key.push_back(')');
            return Key;
          }
        },
        Node.Node);
  }

  static bool isComparisonOp(BinaryOp Op) noexcept {
    return Op == BinaryOp::Eq || Op == BinaryOp::Ne || Op == BinaryOp::Lt ||
           Op == BinaryOp::Le || Op == BinaryOp::Gt || Op == BinaryOp::Ge;
  }

  static bool isPriceSeriesExpr(const Expr *Node) noexcept {
    if (Node == nullptr) {
      return false;
    }
    if (const auto *Id = std::get_if<IdentExpr>(&Node->Node)) {
      return isPriceSeries(Id->Name);
    }
    if (const auto *Offset = std::get_if<OffsetExpr>(&Node->Node)) {
      if (Offset->Target == nullptr) {
        return false;
      }
      if (const auto *Id = std::get_if<IdentExpr>(&Offset->Target->Node)) {
        return isPriceSeries(Id->Name);
      }
    }
    return false;
  }

  static bool isPriceOrDirectPriceIndicator(const Expr *Node) noexcept {
    if (isPriceSeriesExpr(Node)) {
      return true;
    }
    if (Node == nullptr) {
      return false;
    }
    if (const auto *Off = std::get_if<OffsetExpr>(&Node->Node)) {
      Node = Off->Target.get();
      if (Node == nullptr) {
        return false;
      }
    }
    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      if ((Call->Callee == "vwap" || Call->Callee == "pivot" ||
           Call->Callee == "pivot_r1" || Call->Callee == "pivot_r2" ||
           Call->Callee == "pivot_s1" || Call->Callee == "pivot_s2") &&
          Call->Args.size() == 1) {
        return true;
      }
      if ((Call->Callee == "supertrend" || Call->Callee == "keltner_upper" ||
           Call->Callee == "keltner_lower" || Call->Callee == "swing_high" ||
           Call->Callee == "swing_low") &&
          Call->Args.size() == 2) {
        return true;
      }
      return isDirectPriceIndicatorName(Call->Callee) &&
             Call->Args.size() >= 2 && isPriceSeriesExpr(Call->Args[0].get());
    }
    return false;
  }

  bool isConstantLevelExpr(const Expr *Node) const {
    if (Node == nullptr) {
      return false;
    }
    if (std::holds_alternative<NumberLitExpr>(Node->Node)) {
      return true;
    }
    if (const auto *Id = std::get_if<IdentExpr>(&Node->Node)) {
      return State.ParamMap.contains(Id->Name);
    }
    if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
      return Unary->Op == UnaryOp::Neg &&
             isConstantLevelExpr(Unary->Operand.get());
    }
    if (const auto *Binary = std::get_if<BinaryExpr>(&Node->Node)) {
      const bool IsArith =
          Binary->Op == BinaryOp::Add || Binary->Op == BinaryOp::Sub ||
          Binary->Op == BinaryOp::Mul || Binary->Op == BinaryOp::Div;
      return IsArith && isConstantLevelExpr(Binary->Left.get()) &&
             isConstantLevelExpr(Binary->Right.get());
    }
    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      const bool IsStatelessMath =
          Call->Callee == "abs" || Call->Callee == "min" ||
          Call->Callee == "max" || Call->Callee == "sqrt";
      if (!IsStatelessMath || Call->Args.empty()) {
        return false;
      }
      for (const auto &Arg : Call->Args) {
        if (!isConstantLevelExpr(Arg.get())) {
          return false;
        }
      }
      return true;
    }
    return false;
  }

  void checkLookaheadExpr(const Expr *Node) {
    if (Node == nullptr) {
      return;
    }
    if (const auto *Offset = std::get_if<OffsetExpr>(&Node->Node)) {
      checkLookaheadExpr(Offset->Target.get());
      if (Node->ResolvedType == ValueType::Error) {
        return;
      }
      if (!Offset->OffsetIsIdent) {
        const std::int64_t Eff =
            Offset->HasUnaryMinus ? -Offset->OffsetInt : Offset->OffsetInt;
        if (Eff < 0) {
          emitDiag("E-LA01", DiagnosticSeverity::Error, Node->Range,
                   "Чтение будущего: отрицательное смещение запрещено");
          escalateLookahead(LookaheadLevel::Error);
        }
        return;
      }
      const auto It = State.ParamMap.find(Offset->OffsetIdent);
      if (It == State.ParamMap.end()) {
        return;
      }
      const ResolvedParam &Param = It->second;
      const double Eff =
          Offset->HasUnaryMinus ? -Param.EffectiveValue : Param.EffectiveValue;
      if (Eff < 0.0) {
        emitDiag("E-LA01", DiagnosticSeverity::Error, Node->Range,
                 "Чтение будущего: смещение через параметр даёт "
                 "отрицательное значение");
        escalateLookahead(LookaheadLevel::Error);
        return;
      }
      if (Param.HasRange) {
        const double MinEff =
            Offset->HasUnaryMinus ? -Param.RangeMax : Param.RangeMin;
        if (MinEff < 0.0) {
          emitDiag("E-LA02", DiagnosticSeverity::Error, Node->Range,
                   "Чтение будущего: диапазон параметра допускает "
                   "отрицательное смещение");
          escalateLookahead(LookaheadLevel::Error);
        }
      }
      return;
    }
    if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
      checkLookaheadExpr(Unary->Operand.get());
      return;
    }
    if (const auto *Binary = std::get_if<BinaryExpr>(&Node->Node)) {
      checkLookaheadExpr(Binary->Left.get());
      checkLookaheadExpr(Binary->Right.get());
      if (Node->ResolvedType != ValueType::Error &&
          isComparisonOp(Binary->Op) && Binary->Left && Binary->Right &&
          Binary->Left->ResolvedType != ValueType::Error &&
          Binary->Right->ResolvedType != ValueType::Error) {
        const bool Triggered =
            (isPriceOrDirectPriceIndicator(Binary->Left.get()) &&
             isConstantLevelExpr(Binary->Right.get())) ||
            (isPriceOrDirectPriceIndicator(Binary->Right.get()) &&
             isConstantLevelExpr(Binary->Left.get()));
        if (Triggered) {
          emitDiag("W-LA01", DiagnosticSeverity::Warning, Node->Range,
                   "Сравнение цены с абсолютным уровнем может указывать на "
                   "подгонку под историю");
          escalateLookahead(LookaheadLevel::Warning);
        }
      }
      return;
    }
    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      for (const auto &Arg : Call->Args) {
        checkLookaheadExpr(Arg.get());
      }
      if (Node->ResolvedType != ValueType::Error &&
          (Call->Callee == "crossover" || Call->Callee == "crossunder") &&
          Call->Args.size() == 2 && Call->Args[0] && Call->Args[1] &&
          Call->Args[0]->ResolvedType != ValueType::Error &&
          Call->Args[1]->ResolvedType != ValueType::Error) {
        const bool Triggered =
            (isPriceOrDirectPriceIndicator(Call->Args[0].get()) &&
             isConstantLevelExpr(Call->Args[1].get())) ||
            (isPriceOrDirectPriceIndicator(Call->Args[1].get()) &&
             isConstantLevelExpr(Call->Args[0].get()));
        if (Triggered) {
          emitDiag("W-LA01", DiagnosticSeverity::Warning, Node->Range,
                   "Пересечение цены с абсолютным уровнем может указывать на "
                   "подгонку под историю");
          escalateLookahead(LookaheadLevel::Warning);
        }
      }
    }
  }

  void analyzeLookahead(const Program &Ast) {
    for (const auto &Rule : Ast.Rules) {
      checkLookaheadExpr(Rule.Condition.get());
      checkLookaheadExpr(Rule.Action.SizeExpr.get());
      checkLookaheadExpr(Rule.Action.PriceExpr.get());
      if (Rule.Because.Present) {
        for (const auto &Part : Rule.Because.Parts) {
          if (Part.IsExpr) {
            checkLookaheadExpr(Part.Expression.get());
          }
        }
      }
    }
  }

  AnalysisState State;
  std::map<std::string, ParamMeta> MetaMap;
  std::set<std::string> UniqueDagKeys;
  bool DagLimitReported = false;
  std::vector<Diagnostic> &OutDiagnostics;
};

} // namespace

bool isBuiltinName(std::string_view Name) {
  return isMarketSeries(Name) || isPortfolioVar(Name) ||
         getBuiltinFuncSpec(Name).has_value();
}

AnalysisState
analyzeSemantics(Program &Ast,
                 const std::map<std::string, double> &ParamOverrides,
                 std::vector<Diagnostic> &Diagnostics) {
  SemanticAnalyzer Analyzer(Diagnostics);
  return Analyzer.run(Ast, ParamOverrides);
}

} // namespace dsl::internal
