#include "dsl/interpreter.hpp"

#include "dsl/ast.hpp"
#include "dsl/check.hpp"
#include "dsl/types.hpp"
#include "internal/analysis.hpp"
#include "internal/compiled_program.hpp"
#include "internal/indicators.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dsl {
namespace {

using internal::CompiledAction;
using internal::CompiledBecausePart;
using internal::CompiledExpr;
using internal::CompiledExprOp;
using internal::CompiledRule;
using internal::DagInputDesc;
using internal::DagInputKind;
using internal::DagNodeKind;
using internal::DagNodeSpec;
using internal::InstrumentState;
using internal::kInvalidExprId;
using internal::kSeriesCount;
using internal::PortfolioVarKind;
using internal::PureCallKind;
using internal::RingBuffer;
using internal::SeriesKind;

std::optional<std::size_t> seriesIndex(std::string_view Name) noexcept {
  if (Name == "open") {
    return 0;
  }
  if (Name == "high") {
    return 1;
  }
  if (Name == "low") {
    return 2;
  }
  if (Name == "close") {
    return 3;
  }
  if (Name == "volume") {
    return 4;
  }
  return std::nullopt;
}

SeriesKind seriesKindFromIndex(std::size_t Idx) noexcept {
  switch (Idx) {
  case 0:
    return SeriesKind::Open;
  case 1:
    return SeriesKind::High;
  case 2:
    return SeriesKind::Low;
  case 3:
    return SeriesKind::Close;
  default:
    return SeriesKind::Volume;
  }
}

std::optional<PortfolioVarKind>
portfolioVarKind(std::string_view Name) noexcept {
  if (Name == "position") {
    return PortfolioVarKind::Position;
  }
  if (Name == "entry_price") {
    return PortfolioVarKind::EntryPrice;
  }
  if (Name == "cash") {
    return PortfolioVarKind::Cash;
  }
  if (Name == "equity") {
    return PortfolioVarKind::Equity;
  }
  if (Name == "bars_in_position") {
    return PortfolioVarKind::BarsInPosition;
  }
  return std::nullopt;
}

std::optional<DagNodeKind> dagNodeKind(std::string_view Name) noexcept {
  if (Name == "sma") {
    return DagNodeKind::Sma;
  }
  if (Name == "ema") {
    return DagNodeKind::Ema;
  }
  if (Name == "rsi") {
    return DagNodeKind::Rsi;
  }
  if (Name == "atr") {
    return DagNodeKind::Atr;
  }
  if (Name == "stdev") {
    return DagNodeKind::Stdev;
  }
  if (Name == "highest") {
    return DagNodeKind::Highest;
  }
  if (Name == "lowest") {
    return DagNodeKind::Lowest;
  }
  if (Name == "change") {
    return DagNodeKind::Change;
  }
  if (Name == "crossover") {
    return DagNodeKind::Crossover;
  }
  if (Name == "crossunder") {
    return DagNodeKind::Crossunder;
  }
  return std::nullopt;
}

bool isBoolDagKind(DagNodeKind Kind) noexcept {
  return Kind == DagNodeKind::Crossover || Kind == DagNodeKind::Crossunder;
}

std::optional<PureCallKind> pureCallKind(std::string_view Name) noexcept {
  if (Name == "abs") {
    return PureCallKind::Abs;
  }
  if (Name == "min") {
    return PureCallKind::Min;
  }
  if (Name == "max") {
    return PureCallKind::Max;
  }
  if (Name == "sqrt") {
    return PureCallKind::Sqrt;
  }
  if (Name == "is_na") {
    return PureCallKind::IsNa;
  }
  return std::nullopt;
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

std::string encodeDouble(double Val) {
  const double Norm = (Val == 0.0) ? 0.0 : Val;
  return "N:" + std::to_string(std::bit_cast<std::uint64_t>(Norm));
}

class CompilerBuilder {
public:
  CompilerBuilder(const internal::AnalysisState &StateIn,
                  StrategyInstance::Impl &OutIn)
      : State(StateIn), Out(OutIn) {}

  bool build(std::size_t InstrumentCount) {
    for (const auto &Rule : Out.OwnedAst.Rules) {
      collectDag(Rule.Condition.get());
      collectDag(Rule.Action.SizeExpr.get());
      collectDag(Rule.Action.PriceExpr.get());
      if (Rule.Because.Present) {
        for (const auto &Part : Rule.Because.Parts) {
          if (Part.IsExpr) {
            collectDag(Part.Expression.get());
          }
        }
      }
      if (DagLimitExceeded) {
        return false;
      }
    }

    Out.Rules.reserve(Out.OwnedAst.Rules.size());
    for (std::size_t I = 0; I < Out.OwnedAst.Rules.size(); ++I) {
      const auto &Rule = Out.OwnedAst.Rules[I];
      CompiledRule CRule;
      CRule.RuleId = I;
      CRule.Name = trimWhitespace(Rule.Name);
      if (Rule.Condition) {
        CRule.ConditionExprId = compileExpr(*Rule.Condition);
      }
      CRule.Action.Kind = Rule.Action.Kind;
      CRule.Action.Unit = Rule.Action.Unit;
      CRule.Action.OrderKind = Rule.Action.OrderKind;
      CRule.Action.Tif = Rule.Action.Tif;
      if (Rule.Action.SizeExpr) {
        CRule.Action.SizeExprId = compileExpr(*Rule.Action.SizeExpr);
      }
      if (Rule.Action.PriceExpr) {
        CRule.Action.PriceExprId = compileExpr(*Rule.Action.PriceExpr);
      }
      if (Rule.Because.Present) {
        CRule.BecauseParts.reserve(Rule.Because.Parts.size());
        for (const auto &Part : Rule.Because.Parts) {
          CompiledBecausePart CPart;
          CPart.IsExpr = Part.IsExpr;
          CPart.LiteralText = Part.LiteralText;
          if (Part.IsExpr && Part.Expression) {
            CPart.IsBoolExpr =
                (Part.Expression->ResolvedType == ValueType::Bool);
            CPart.ExprId = compileExpr(*Part.Expression);
          }
          CRule.BecauseParts.push_back(std::move(CPart));
        }
      }
      Out.Rules.push_back(std::move(CRule));
    }

    finalizeBuffers(InstrumentCount);
    return true;
  }

private:
  double resolveParam(const std::string &Name) const {
    const auto It = State.ParamMap.find(Name);
    if (It == State.ParamMap.end()) {
      return 0.0;
    }
    return It->second.EffectiveValue;
  }

  std::size_t resolveOffset(const OffsetExpr &Offset) const {
    if (!Offset.OffsetIsIdent) {
      const std::int64_t Eff =
          Offset.HasUnaryMinus ? -Offset.OffsetInt : Offset.OffsetInt;
      return static_cast<std::size_t>(std::max<std::int64_t>(0, Eff));
    }
    const double Raw = resolveParam(Offset.OffsetIdent);
    const double SignedVal = Offset.HasUnaryMinus ? -Raw : Raw;
    if (!std::isfinite(SignedVal) || SignedVal <= 0.0) {
      return 0;
    }
    return static_cast<std::size_t>(SignedVal);
  }

  std::size_t resolvePeriod(const Expr &Arg) const {
    if (const auto *Lit = std::get_if<NumberLitExpr>(&Arg.Node)) {
      if (!std::isfinite(Lit->Value) || Lit->Value < 1.0) {
        return 1;
      }
      return static_cast<std::size_t>(Lit->Value);
    }
    if (const auto *Id = std::get_if<IdentExpr>(&Arg.Node)) {
      const double Val = resolveParam(Id->Name);
      if (!std::isfinite(Val) || Val < 1.0) {
        return 1;
      }
      return static_cast<std::size_t>(Val);
    }
    return 1;
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
            const std::size_t Eff = resolveOffset(Inner);
            std::string TargetKey;
            if (Inner.Target) {
              TargetKey = canonicalExprKey(*Inner.Target);
            }
            if (Eff == 0) {
              return TargetKey;
            }
            return "O(" + TargetKey + "," + std::to_string(Eff) + ")";
          } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            const std::string OpKey =
                Inner.Op == UnaryOp::Neg ? "neg" : "not";
            const std::string SubKey =
                Inner.Operand ? canonicalExprKey(*Inner.Operand)
                              : std::string{};
            return "U(" + OpKey + "," + SubKey + ")";
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

  void collectDag(const Expr *Node) {
    if (Node == nullptr || DagLimitExceeded) {
      return;
    }
    if (const auto *Offset = std::get_if<OffsetExpr>(&Node->Node)) {
      collectDag(Offset->Target.get());
      return;
    }
    if (const auto *Unary = std::get_if<UnaryExpr>(&Node->Node)) {
      collectDag(Unary->Operand.get());
      return;
    }
    if (const auto *Binary = std::get_if<BinaryExpr>(&Node->Node)) {
      collectDag(Binary->Left.get());
      collectDag(Binary->Right.get());
      return;
    }
    if (const auto *Call = std::get_if<CallExpr>(&Node->Node)) {
      for (const auto &Arg : Call->Args) {
        collectDag(Arg.get());
        if (DagLimitExceeded) {
          return;
        }
      }
      const auto Kind = dagNodeKind(Call->Callee);
      if (!Kind.has_value()) {
        return;
      }
      const std::string Key = canonicalExprKey(*Node);
      if (const auto It = KeyToDagIdx.find(Key); It != KeyToDagIdx.end()) {
        ExprToDagIdx.emplace(Node, It->second);
        return;
      }
      if (Out.DagNodes.size() >= kMaxDagNodes) {
        DagLimitExceeded = true;
        return;
      }

      const std::size_t NodeIdx = Out.DagNodes.size();
      DagNodeSpec Spec;
      Spec.Kind = *Kind;
      Spec.CanonicalKey = Key;
      Spec.OutputCapacity = 1;
      std::size_t ExprIdA = kInvalidExprId;
      std::size_t ExprIdB = kInvalidExprId;

      if (*Kind == DagNodeKind::Atr) {
        Spec.Period = resolvePeriod(*Call->Args[0]);
      } else if (*Kind == DagNodeKind::Crossover ||
                 *Kind == DagNodeKind::Crossunder) {
        Spec.Period = 0;
        Spec.InputA = buildDagInput(*Call->Args[0], ExprIdA);
        Spec.InputB = buildDagInput(*Call->Args[1], ExprIdB);
      } else {
        Spec.Period = resolvePeriod(*Call->Args[1]);
        Spec.InputA = buildDagInput(*Call->Args[0], ExprIdA);
      }

      KeyToDagIdx.emplace(Key, NodeIdx);
      ExprToDagIdx.emplace(Node, NodeIdx);
      Out.DagNodes.push_back(std::move(Spec));
      Out.DagInputExprA.push_back(ExprIdA);
      Out.DagInputExprB.push_back(ExprIdB);
      DagMaxOffset.push_back(0);
    }
  }

  DagInputDesc buildDagInput(const Expr &Arg, std::size_t &OutExprId) {
    DagInputDesc Desc;
    if (const auto *Lit = std::get_if<NumberLitExpr>(&Arg.Node)) {
      Desc.Kind = DagInputKind::Constant;
      Desc.ConstantValue = Lit->Value;
      return Desc;
    }
    if (const auto *Id = std::get_if<IdentExpr>(&Arg.Node)) {
      if (const auto SIdx = seriesIndex(Id->Name); SIdx.has_value()) {
        Desc.Kind = DagInputKind::Series;
        Desc.Series = seriesKindFromIndex(*SIdx);
        Desc.Offset = 0;
        return Desc;
      }
      Desc.Kind = DagInputKind::Constant;
      Desc.ConstantValue = resolveParam(Id->Name);
      return Desc;
    }
    if (const auto *Offset = std::get_if<OffsetExpr>(&Arg.Node)) {
      const std::size_t Off = resolveOffset(*Offset);
      if (const auto *Id = std::get_if<IdentExpr>(&Offset->Target->Node)) {
        const std::size_t SIdx = seriesIndex(Id->Name).value();
        SeriesMaxOffset[SIdx] = std::max(SeriesMaxOffset[SIdx], Off);
        Desc.Kind = DagInputKind::Series;
        Desc.Series = seriesKindFromIndex(SIdx);
        Desc.Offset = Off;
        return Desc;
      }
      const std::size_t NodeIdx = ExprToDagIdx.at(Offset->Target.get());
      DagMaxOffset[NodeIdx] = std::max(DagMaxOffset[NodeIdx], Off);
      Desc.Kind = DagInputKind::DagNode;
      Desc.NodeIndex = NodeIdx;
      Desc.Offset = Off;
      return Desc;
    }
    if (const auto It = ExprToDagIdx.find(&Arg); It != ExprToDagIdx.end()) {
      Desc.Kind = DagInputKind::DagNode;
      Desc.NodeIndex = It->second;
      Desc.Offset = 0;
      return Desc;
    }
    OutExprId = compileExpr(Arg);
    Desc.Kind = DagInputKind::ExprAst;
    Desc.AstExpr = &Arg;
    return Desc;
  }

  std::size_t pushExpr(const CompiledExpr &Node) {
    const std::size_t Id = Out.ExprPool.size();
    Out.ExprPool.push_back(Node);
    return Id;
  }

  std::size_t compileExpr(const Expr &Node) {
    return std::visit(
        [this, &Node](const auto &Inner) -> std::size_t {
          using T = std::decay_t<decltype(Inner)>;
          CompiledExpr OutNode;
          if constexpr (std::is_same_v<T, NumberLitExpr>) {
            OutNode.Op = CompiledExprOp::NumberLit;
            OutNode.NumberVal = Inner.Value;
            return pushExpr(OutNode);
          } else if constexpr (std::is_same_v<T, BoolLitExpr>) {
            OutNode.Op = CompiledExprOp::BoolLit;
            OutNode.BoolVal = Inner.Value;
            return pushExpr(OutNode);
          } else if constexpr (std::is_same_v<T, IdentExpr>) {
            if (const auto SIdx = seriesIndex(Inner.Name); SIdx.has_value()) {
              OutNode.Op = CompiledExprOp::Series;
              OutNode.Index = *SIdx;
              OutNode.Offset = 0;
              return pushExpr(OutNode);
            }
            if (const auto PKind = portfolioVarKind(Inner.Name);
                PKind.has_value()) {
              OutNode.Op = CompiledExprOp::PortfolioVar;
              OutNode.VarKind = *PKind;
              return pushExpr(OutNode);
            }
            OutNode.Op = CompiledExprOp::NumberLit;
            OutNode.NumberVal = resolveParam(Inner.Name);
            return pushExpr(OutNode);
          } else if constexpr (std::is_same_v<T, OffsetExpr>) {
            const std::size_t Off = resolveOffset(Inner);
            if (const auto *Id =
                    std::get_if<IdentExpr>(&Inner.Target->Node)) {
              const std::size_t SIdx = seriesIndex(Id->Name).value();
              SeriesMaxOffset[SIdx] = std::max(SeriesMaxOffset[SIdx], Off);
              OutNode.Op = CompiledExprOp::Series;
              OutNode.Index = SIdx;
              OutNode.Offset = Off;
              return pushExpr(OutNode);
            }
            const std::size_t NodeIdx = ExprToDagIdx.at(Inner.Target.get());
            DagMaxOffset[NodeIdx] = std::max(DagMaxOffset[NodeIdx], Off);
            OutNode.Op = CompiledExprOp::DagNode;
            OutNode.Index = NodeIdx;
            OutNode.Offset = Off;
            OutNode.BoolVal = false;
            return pushExpr(OutNode);
          } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            OutNode.Op = (Inner.Op == UnaryOp::Neg) ? CompiledExprOp::Neg
                                                    : CompiledExprOp::Not;
            OutNode.Left = compileExpr(*Inner.Operand);
            return pushExpr(OutNode);
          } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            OutNode.Op = CompiledExprOp::Binary;
            OutNode.BinOp = Inner.Op;
            OutNode.Left = compileExpr(*Inner.Left);
            OutNode.Right = compileExpr(*Inner.Right);
            OutNode.BoolOperands =
                (Inner.Left->ResolvedType == ValueType::Bool);
            return pushExpr(OutNode);
          } else {
            if (const auto It = ExprToDagIdx.find(&Node);
                It != ExprToDagIdx.end()) {
              OutNode.Op = CompiledExprOp::DagNode;
              OutNode.Index = It->second;
              OutNode.Offset = 0;
              OutNode.BoolVal =
                  isBoolDagKind(Out.DagNodes[It->second].Kind);
              return pushExpr(OutNode);
            }
            OutNode.Op = CompiledExprOp::PureCall;
            OutNode.PureKind = pureCallKind(Inner.Callee).value();
            OutNode.Left = compileExpr(*Inner.Args[0]);
            if (Inner.Args.size() > 1) {
              OutNode.Right = compileExpr(*Inner.Args[1]);
            }
            return pushExpr(OutNode);
          }
        },
        Node.Node);
  }

  void finalizeBuffers(std::size_t InstrumentCount) {
    std::size_t MaxLb = 0;
    for (std::size_t I = 0; I < kSeriesCount; ++I) {
      Out.SeriesCaps[I] = SeriesMaxOffset[I] + 1;
      MaxLb = std::max(MaxLb, SeriesMaxOffset[I]);
    }
    for (std::size_t D = 0; D < Out.DagNodes.size(); ++D) {
      Out.DagNodes[D].OutputCapacity = DagMaxOffset[D] + 1;
      MaxLb = std::max(MaxLb, DagMaxOffset[D]);
    }
    Out.MaxLookback = MaxLb;

    Out.Instruments.resize(InstrumentCount);
    for (InstrumentState &Inst : Out.Instruments) {
      for (std::size_t I = 0; I < kSeriesCount; ++I) {
        Inst.Series[I] = RingBuffer(Out.SeriesCaps[I]);
      }
      Inst.DagStates.reserve(Out.DagNodes.size());
      for (const DagNodeSpec &Spec : Out.DagNodes) {
        Inst.DagStates.push_back(internal::makeDagNodeState(Spec));
      }
    }
  }

  const internal::AnalysisState &State;
  StrategyInstance::Impl &Out;
  std::map<std::string, std::size_t> KeyToDagIdx;
  std::map<const Expr *, std::size_t> ExprToDagIdx;
  std::array<std::size_t, kSeriesCount> SeriesMaxOffset = {0, 0, 0, 0, 0};
  std::vector<std::size_t> DagMaxOffset;
  bool DagLimitExceeded = false;
};

} // namespace

CompileResult compile(std::string_view Source,
                      const std::map<std::string, double> &ParamOverrides,
                      std::size_t InstrumentCount) {
  CompileResult Result;
  ParseResult Parsed = parse(Source);
  Result.Diagnostics = std::move(Parsed.Diagnostics);
  if (Parsed.HasSyntaxErrors) {
    sortDiagnostics(Result.Diagnostics);
    return Result;
  }

  internal::AnalysisState State = internal::analyzeSemantics(
      Parsed.Ast, ParamOverrides, Result.Diagnostics);
  for (const Diagnostic &Diag : Result.Diagnostics) {
    if (Diag.Severity == DiagnosticSeverity::Error) {
      sortDiagnostics(Result.Diagnostics);
      return Result;
    }
  }

  auto ImplPtr = std::make_unique<StrategyInstance::Impl>();
  ImplPtr->OwnedAst = std::move(Parsed.Ast);
  ImplPtr->Params = std::move(State.ParamDecls);

  CompilerBuilder Builder(State, *ImplPtr);
  if (!Builder.build(InstrumentCount)) {
    Diagnostic LimitDiag;
    LimitDiag.Code = "E-SYN";
    LimitDiag.Severity = DiagnosticSeverity::Error;
    LimitDiag.Message =
        "Превышено максимальное число уникальных узлов индикаторов (256)";
    Result.Diagnostics.push_back(std::move(LimitDiag));
    sortDiagnostics(Result.Diagnostics);
    return Result;
  }

  sortDiagnostics(Result.Diagnostics);
  Result.Instance = std::make_unique<StrategyInstance>(std::move(ImplPtr));
  Result.Ok = true;
  return Result;
}

} // namespace dsl
