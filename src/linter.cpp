#include "internal/analysis.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dsl::internal {
namespace {

struct LineInfo {
  std::size_t StartByte = 0;
  std::size_t EndByte = 0;
  std::size_t CodePointLen = 0;
  bool HasNewline = false;
};

struct AnchorPoint {
  SourcePos Pos;
  std::string Prefix;
  std::string Suffix;
};

struct BuiltinFuncInfo {
  std::size_t Arity = 0;
  std::uint8_t PeriodArgMask = 0;

  bool isPeriodArg(std::size_t Index) const noexcept {
    return Index < 8U && ((PeriodArgMask & (1U << Index)) != 0U);
  }
};

std::vector<LineInfo> buildLineIndex(std::string_view Source) {
  std::vector<LineInfo> Lines;
  std::size_t Index = 0;
  while (Index < Source.size()) {
    const std::size_t LineStart = Index;
    std::size_t ColCount = 0;
    while (Index < Source.size() && Source[Index] != '\n' &&
           Source[Index] != '\r') {
      const auto Byte = static_cast<unsigned char>(Source[Index]);
      if ((Byte & 0xC0U) != 0x80U) {
        ++ColCount;
      }
      ++Index;
    }
    const std::size_t LineEnd = Index;
    bool HasNl = false;
    if (Index < Source.size()) {
      HasNl = true;
      if (Source[Index] == '\r' && Index + 1 < Source.size() &&
          Source[Index + 1] == '\n') {
        Index += 2;
      } else {
        ++Index;
      }
    }
    Lines.push_back(LineInfo{LineStart, LineEnd, ColCount, HasNl});
  }
  if (Lines.empty() || Lines.back().HasNewline) {
    Lines.push_back(LineInfo{Index, Index, 0, false});
  }
  return Lines;
}

std::size_t posToByteOffset(std::string_view Source,
                            const std::vector<LineInfo> &Lines, SourcePos Pos) {
  if (Pos.Line == 0 || Pos.Line > Lines.size()) {
    return Source.size();
  }
  const LineInfo &Info = Lines[Pos.Line - 1];
  if (Pos.Column <= 1) {
    return Info.StartByte;
  }
  std::size_t BytePos = Info.StartByte;
  std::size_t Col = 1;
  while (BytePos < Info.EndByte && Col < Pos.Column) {
    ++BytePos;
    while (BytePos < Info.EndByte &&
           (static_cast<unsigned char>(Source[BytePos]) & 0xC0U) == 0x80U) {
      ++BytePos;
    }
    ++Col;
  }
  return BytePos;
}

std::string sliceSource(std::string_view Source,
                        const std::vector<LineInfo> &Lines,
                        const SourceRange &Range) {
  const std::size_t StartOff = posToByteOffset(Source, Lines, Range.Start);
  const std::size_t EndOff = posToByteOffset(Source, Lines, Range.End);
  if (StartOff >= EndOff || EndOff > Source.size()) {
    return {};
  }
  return std::string(Source.substr(StartOff, EndOff - StartOff));
}

bool isHorizontalSpace(char Ch) noexcept { return Ch == ' ' || Ch == '\t'; }

AnchorPoint computeAnchor(const Program &Ast, std::string_view Source,
                          const std::vector<LineInfo> &Lines) {
  if (!Ast.Params.empty()) {
    const ParamStmt &LastParam = Ast.Params.back();
    const SourcePos EndPos = LastParam.Range.End;
    if (EndPos.Line >= 1 && EndPos.Line <= Lines.size()) {
      if (EndPos.Column == 1 && EndPos.Line > LastParam.Range.Start.Line) {
        return AnchorPoint{EndPos, "", "\n"};
      }
      const LineInfo &Info = Lines[EndPos.Line - 1];
      std::size_t Idx = posToByteOffset(Source, Lines, EndPos);
      while (Idx < Info.EndByte && isHorizontalSpace(Source[Idx])) {
        ++Idx;
      }
      if (Idx == Info.EndByte || Source[Idx] == '#') {
        if (Info.HasNewline) {
          return AnchorPoint{SourcePos{EndPos.Line + 1, 1}, "", "\n"};
        }
        return AnchorPoint{SourcePos{EndPos.Line, Info.CodePointLen + 1}, "\n",
                           "\n"};
      }
      return AnchorPoint{EndPos, "\n", "\n"};
    }
    return AnchorPoint{SourcePos{Lines.size(), 1}, "\n", "\n"};
  }

  if (Ast.Rules.empty()) {
    return AnchorPoint{SourcePos{1, 1}, "", "\n"};
  }
  const SourcePos RuleStart = Ast.Rules.front().Range.Start;
  if (RuleStart.Line >= 1 && RuleStart.Line <= Lines.size()) {
    const LineInfo &Info = Lines[RuleStart.Line - 1];
    const std::size_t RuleOff = posToByteOffset(Source, Lines, RuleStart);
    bool OnlySpaceBefore = true;
    for (std::size_t I = Info.StartByte; I < RuleOff; ++I) {
      if (!isHorizontalSpace(Source[I])) {
        OnlySpaceBefore = false;
        break;
      }
    }
    if (OnlySpaceBefore) {
      return AnchorPoint{SourcePos{RuleStart.Line, 1}, "", "\n"};
    }
    return AnchorPoint{RuleStart, "\n", "\n"};
  }
  return AnchorPoint{SourcePos{1, 1}, "", "\n"};
}

bool isKeyword(std::string_view Name) noexcept {
  return Name == "param" || Name == "range" || Name == "step" ||
         Name == "rule" || Name == "when" || Name == "then" ||
         Name == "because" || Name == "buy" || Name == "sell" ||
         Name == "cancel" || Name == "lots" || Name == "all" ||
         Name == "market" || Name == "limit" || Name == "stop" ||
         Name == "take_profit" || Name == "day" || Name == "gtc" ||
         Name == "and" || Name == "or" || Name == "not" || Name == "true" ||
         Name == "false";
}

std::optional<BuiltinFuncInfo> getBuiltinFuncInfo(std::string_view Name) {
  if (Name == "obv" || Name == "is_doji" || Name == "is_hammer" ||
      Name == "is_shooting_star" || Name == "is_bullish_engulfing" ||
      Name == "is_bearish_engulfing" || Name == "is_inside_bar" ||
      Name == "is_pin_bar") {
    return BuiltinFuncInfo{0, 0U};
  }
  if (Name == "atr" || Name == "vwap" || Name == "stoch_k" ||
      Name == "stoch_rsi" || Name == "adx" || Name == "plus_di" ||
      Name == "minus_di" || Name == "cci" || Name == "willr" ||
      Name == "bull_power" || Name == "bear_power" || Name == "mfi" ||
      Name == "pivot" || Name == "pivot_r1" || Name == "pivot_r2" ||
      Name == "pivot_s1" || Name == "pivot_s2") {
    return BuiltinFuncInfo{1, 1U << 0U};
  }
  if (Name == "sma" || Name == "ema" || Name == "wma" || Name == "rsi" ||
      Name == "stdev" || Name == "highest" || Name == "lowest" ||
      Name == "change" || Name == "bb_mid" || Name == "roc" ||
      Name == "linreg" || Name == "slope" || Name == "rising" ||
      Name == "falling") {
    return BuiltinFuncInfo{2, 1U << 1U};
  }
  if (Name == "supertrend" || Name == "keltner_upper" ||
      Name == "keltner_lower") {
    return BuiltinFuncInfo{2, 1U << 0U};
  }
  if (Name == "stoch_d" || Name == "swing_high" || Name == "swing_low") {
    return BuiltinFuncInfo{2, (1U << 0U) | (1U << 1U)};
  }
  if (Name == "bb_upper" || Name == "bb_lower") {
    return BuiltinFuncInfo{3, 1U << 1U};
  }
  if (Name == "macd") {
    return BuiltinFuncInfo{3, (1U << 1U) | (1U << 2U)};
  }
  if (Name == "ult_osc") {
    return BuiltinFuncInfo{3, (1U << 0U) | (1U << 1U) | (1U << 2U)};
  }
  if (Name == "macd_signal" || Name == "macd_hist") {
    return BuiltinFuncInfo{4, (1U << 1U) | (1U << 2U) | (1U << 3U)};
  }
  if (Name == "crossover" || Name == "crossunder" || Name == "min" ||
      Name == "max") {
    return BuiltinFuncInfo{2, 0U};
  }
  if (Name == "abs" || Name == "sqrt" || Name == "is_na" ||
      Name == "bars_since") {
    return BuiltinFuncInfo{1, 0U};
  }
  return std::nullopt;
}

bool isComparisonOp(BinaryOp Op) noexcept {
  return Op == BinaryOp::Eq || Op == BinaryOp::Ne || Op == BinaryOp::Lt ||
         Op == BinaryOp::Le || Op == BinaryOp::Gt || Op == BinaryOp::Ge;
}

bool isLiteralOrNegLiteral(const Expr *Node) noexcept {
  if (Node == nullptr) {
    return false;
  }
  if (std::holds_alternative<NumberLitExpr>(Node->Node)) {
    return true;
  }
  if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
    return Unary->Op == UnaryOp::Neg && Unary->Operand != nullptr &&
           std::holds_alternative<NumberLitExpr>(Unary->Operand->Node);
  }
  return false;
}

std::optional<std::string> resolvedBuiltinCallName(const Expr *Node) {
  if (Node == nullptr) {
    return std::nullopt;
  }
  const auto *Call = std::get_if<CallExpr>(&Node->Node);
  if (Call == nullptr) {
    return std::nullopt;
  }
  const auto Info = getBuiltinFuncInfo(Call->Callee);
  if (!Info.has_value() || Call->Args.size() != Info->Arity) {
    return std::nullopt;
  }
  return Call->Callee;
}

bool isExcludedNumericValue(double Value) noexcept {
  return Value == 0.0 || Value == 1.0 || Value == 100.0;
}

class LinterContext {
public:
  LinterContext(const Program &Ast, std::string_view Source,
                const AnalysisState &State,
                std::vector<Diagnostic> &Diagnostics)
      : SourceText(Source), LineIndex(buildLineIndex(Source)),
        Anchor(computeAnchor(Ast, Source, LineIndex)),
        OutDiagnostics(Diagnostics) {
    for (const auto &Param : Ast.Params) {
      ReservedNames.insert(Param.Name);
    }
    for (const auto &Entry : State.ParamMap) {
      ReservedNames.insert(Entry.first);
    }
    const std::size_t ExistingParams =
        std::max(Ast.Params.size(), State.ParamMap.size());
    if (ExistingParams < kMaxParams) {
      RemainingSlots = kMaxParams - ExistingParams;
    }
  }

  void lintRule(const RuleStmt &Rule) {
    lintExpr(Rule.Condition.get(), std::nullopt);
    lintExpr(Rule.Action.SizeExpr.get(), std::nullopt);
    lintExpr(Rule.Action.PriceExpr.get(), std::nullopt);
    if (Rule.Because.Present) {
      for (const auto &Part : Rule.Because.Parts) {
        if (Part.IsExpr) {
          lintExpr(Part.Expression.get(), std::nullopt);
        }
      }
    }
  }

private:
  bool isNameTaken(std::string_view Name) const {
    return ReservedNames.find(std::string(Name)) != ReservedNames.end() ||
           isBuiltinName(Name) || isKeyword(Name);
  }

  std::string allocateParamName(const std::optional<std::string> &BaseName) {
    std::string Base;
    if (BaseName.has_value()) {
      Base = *BaseName;
    } else {
      Base = "p" + std::to_string(GenericCounter++);
    }
    std::string Candidate = Base;
    std::size_t Suffix = 2;
    while (isNameTaken(Candidate)) {
      Candidate = Base + "_" + std::to_string(Suffix++);
    }
    ReservedNames.insert(Candidate);
    return Candidate;
  }

  void emitMagicNumber(const SourceRange &LitRange, std::string LiteralText,
                       const std::optional<std::string> &ContextBaseName) {
    Diagnostic Diag;
    Diag.Code = "L001";
    Diag.Severity = DiagnosticSeverity::Warning;
    Diag.Range = LitRange;
    Diag.Message =
        "Магическая константа `" + LiteralText + "` — вынесите в параметр";

    if (RemainingSlots > 0) {
      const std::string ParamName = allocateParamName(ContextBaseName);
      DiagnosticFix Fix;
      Fix.Description = "Вынести константу в параметр " + ParamName;

      TextEdit InsertEdit;
      InsertEdit.Range = SourceRange{Anchor.Pos, Anchor.Pos};
      InsertEdit.Replacement = Anchor.Prefix + "param " + ParamName + " = " +
                               LiteralText + Anchor.Suffix;

      TextEdit ReplaceEdit;
      ReplaceEdit.Range = LitRange;
      ReplaceEdit.Replacement = ParamName;

      Fix.Edits.push_back(std::move(InsertEdit));
      Fix.Edits.push_back(std::move(ReplaceEdit));
      Diag.Fixes.push_back(std::move(Fix));
      --RemainingSlots;
    }

    OutDiagnostics.push_back(std::move(Diag));
  }

  void lintExpr(const Expr *Node,
                const std::optional<std::string> &ContextBaseName) {
    if (Node == nullptr) {
      return;
    }

    if (const auto *Lit = std::get_if<NumberLitExpr>(&Node->Node)) {
      if (!isExcludedNumericValue(Lit->Value)) {
        emitMagicNumber(Node->TokenRange, Lit->RawText, ContextBaseName);
      }
      return;
    }

    if (std::holds_alternative<BoolLitExpr>(Node->Node) ||
        std::holds_alternative<IdentExpr>(Node->Node)) {
      return;
    }

    if (const auto *Offset = std::get_if<OffsetExpr>(&Node->Node)) {
      lintExpr(Offset->Target.get(), std::nullopt);
      if (!Offset->OffsetIsIdent && !Offset->HasUnaryMinus) {
        const double Val = static_cast<double>(Offset->OffsetInt);
        if (!isExcludedNumericValue(Val)) {
          std::string Text =
              sliceSource(SourceText, LineIndex, Offset->OffsetRange);
          if (Text.empty()) {
            Text = std::to_string(Offset->OffsetInt);
          }
          emitMagicNumber(Offset->OffsetRange, std::move(Text), std::nullopt);
        }
      }
      return;
    }

    if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
      if (Unary->Op == UnaryOp::Neg && Unary->Operand != nullptr) {
        if (const auto *Lit =
                std::get_if<NumberLitExpr>(&Unary->Operand->Node)) {
          const double SignedVal = -Lit->Value;
          if (!isExcludedNumericValue(SignedVal)) {
            emitMagicNumber(Node->TokenRange, "-" + Lit->RawText,
                            ContextBaseName);
          }
          return;
        }
      }
      lintExpr(Unary->Operand.get(), std::nullopt);
      return;
    }

    if (const auto *Binary = std::get_if<BinaryExpr>(&Node->Node)) {
      std::optional<std::string> LeftBase;
      std::optional<std::string> RightBase;
      if (isComparisonOp(Binary->Op)) {
        if (isLiteralOrNegLiteral(Binary->Left.get())) {
          if (const auto CallName =
                  resolvedBuiltinCallName(Binary->Right.get())) {
            LeftBase = *CallName + "_level";
          }
        } else if (isLiteralOrNegLiteral(Binary->Right.get())) {
          if (const auto CallName =
                  resolvedBuiltinCallName(Binary->Left.get())) {
            RightBase = *CallName + "_level";
          }
        }
      }
      lintExpr(Binary->Left.get(), LeftBase);
      lintExpr(Binary->Right.get(), RightBase);
      return;
    }

    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      const auto Info = getBuiltinFuncInfo(Call->Callee);
      if (Info.has_value() && Call->Args.size() == Info->Arity) {
        if (Call->Callee == "crossover" || Call->Callee == "crossunder") {
          std::optional<std::string> Arg0Base;
          std::optional<std::string> Arg1Base;
          if (isLiteralOrNegLiteral(Call->Args[0].get())) {
            if (const auto OtherName =
                    resolvedBuiltinCallName(Call->Args[1].get())) {
              Arg0Base = *OtherName + "_level";
            }
          } else if (isLiteralOrNegLiteral(Call->Args[1].get())) {
            if (const auto OtherName =
                    resolvedBuiltinCallName(Call->Args[0].get())) {
              Arg1Base = *OtherName + "_level";
            }
          }
          lintExpr(Call->Args[0].get(), Arg0Base);
          lintExpr(Call->Args[1].get(), Arg1Base);
          return;
        }
        for (std::size_t I = 0; I < Call->Args.size(); ++I) {
          std::optional<std::string> ArgBase;
          if (Info->isPeriodArg(I) && Call->Args[I] != nullptr &&
              std::holds_alternative<NumberLitExpr>(Call->Args[I]->Node)) {
            ArgBase = Call->Callee + "_n";
          }
          lintExpr(Call->Args[I].get(), ArgBase);
        }
        return;
      }
      for (const auto &Arg : Call->Args) {
        lintExpr(Arg.get(), std::nullopt);
      }
    }
  }

  std::string_view SourceText;
  std::vector<LineInfo> LineIndex;
  AnchorPoint Anchor;
  std::vector<Diagnostic> &OutDiagnostics;
  std::set<std::string> ReservedNames;
  std::size_t GenericCounter = 1;
  std::size_t RemainingSlots = 0;
};

} // namespace

void runLinter(const Program &Ast, std::string_view Source,
               const AnalysisState &State,
               std::vector<Diagnostic> &Diagnostics) {
  LinterContext Context(Ast, Source, State, Diagnostics);
  for (const auto &Rule : Ast.Rules) {
    Context.lintRule(Rule);
  }
}

} // namespace dsl::internal
