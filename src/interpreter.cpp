#include "dsl/interpreter.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"
#include "internal/compiled_program.hpp"
#include "internal/format_because.hpp"
#include "internal/indicators.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
using internal::DagNodeSpec;
using internal::InstrumentState;
using internal::kInvalidExprId;
using internal::PortfolioVarKind;
using internal::PureCallKind;
using internal::SeriesKind;

double normalizeNumber(double Value) noexcept {
  return std::isfinite(Value) ? Value : kNa;
}

std::size_t seriesIndexFromKind(SeriesKind Kind) noexcept {
  switch (Kind) {
  case SeriesKind::Open:
    return 0;
  case SeriesKind::High:
    return 1;
  case SeriesKind::Low:
    return 2;
  case SeriesKind::Close:
    return 3;
  case SeriesKind::Volume:
    return 4;
  }
  return 3;
}

class Evaluator {
public:
  Evaluator(const StrategyInstance::Impl &ProgIn,
            const InstrumentState &InstIn,
            const PortfolioSnapshot *PortfolioIn)
      : Prog(ProgIn), Inst(InstIn), Portfolio(PortfolioIn) {}

  double evalDagInput(const DagInputDesc &Desc, std::size_t ExprId) const {
    switch (Desc.Kind) {
    case DagInputKind::None:
      return kNa;
    case DagInputKind::Series: {
      const std::size_t SIdx = seriesIndexFromKind(Desc.Series);
      return Inst.Series[SIdx].at(Desc.Offset);
    }
    case DagInputKind::DagNode:
      return Inst.DagStates[Desc.NodeIndex].Output.at(Desc.Offset);
    case DagInputKind::Constant:
      return Desc.ConstantValue;
    case DagInputKind::ExprAst:
      return evalNumber(ExprId);
    }
    return kNa;
  }

  double evalNumber(std::size_t ExprId) const {
    if (ExprId == kInvalidExprId) {
      return kNa;
    }
    const CompiledExpr &Node = Prog.ExprPool[ExprId];
    switch (Node.Op) {
    case CompiledExprOp::NumberLit:
      return Node.NumberVal;
    case CompiledExprOp::Series:
      return Inst.Series[Node.Index].at(Node.Offset);
    case CompiledExprOp::DagNode:
      return Inst.DagStates[Node.Index].Output.at(Node.Offset);
    case CompiledExprOp::PortfolioVar:
      return evalPortfolioVar(Node.VarKind);
    case CompiledExprOp::Neg: {
      const double Val = evalNumber(Node.Left);
      return std::isfinite(Val) ? normalizeNumber(-Val) : kNa;
    }
    case CompiledExprOp::Binary:
      return evalArithmetic(Node);
    case CompiledExprOp::PureCall:
      return evalPureNumberCall(Node);
    case CompiledExprOp::BoolLit:
    case CompiledExprOp::Not:
      return kNa;
    }
    return kNa;
  }

  bool evalBool(std::size_t ExprId) const {
    if (ExprId == kInvalidExprId) {
      return false;
    }
    const CompiledExpr &Node = Prog.ExprPool[ExprId];
    switch (Node.Op) {
    case CompiledExprOp::BoolLit:
      return Node.BoolVal;
    case CompiledExprOp::DagNode: {
      const double Val = Inst.DagStates[Node.Index].Output.at(Node.Offset);
      return std::isfinite(Val) && Val != 0.0;
    }
    case CompiledExprOp::Not:
      return !evalBool(Node.Left);
    case CompiledExprOp::Binary:
      return evalLogicalOrCmp(Node);
    case CompiledExprOp::PureCall:
      if (Node.PureKind == PureCallKind::IsNa) {
        return isNaValue(evalNumber(Node.Left));
      }
      return false;
    case CompiledExprOp::NumberLit:
    case CompiledExprOp::Series:
    case CompiledExprOp::PortfolioVar:
    case CompiledExprOp::Neg:
      return false;
    }
    return false;
  }

  std::string evalBecause(const CompiledRule &Rule) const {
    std::string Result;
    for (const CompiledBecausePart &Part : Rule.BecauseParts) {
      if (!Part.IsExpr) {
        Result.append(Part.LiteralText);
      } else if (Part.IsBoolExpr) {
        Result.append(internal::formatBoolRu(evalBool(Part.ExprId)));
      } else {
        Result.append(internal::formatNumberHalfUp2(evalNumber(Part.ExprId)));
      }
    }
    return Result;
  }

private:
  double evalPortfolioVar(PortfolioVarKind Kind) const noexcept {
    if (Portfolio == nullptr) {
      return kNa;
    }
    switch (Kind) {
    case PortfolioVarKind::Position:
      return static_cast<double>(Portfolio->Position);
    case PortfolioVarKind::EntryPrice:
      if (Portfolio->Position == 0) {
        return kNa;
      }
      return normalizeNumber(Portfolio->EntryPrice);
    case PortfolioVarKind::Cash:
      return normalizeNumber(Portfolio->Cash);
    case PortfolioVarKind::Equity:
      return normalizeNumber(Portfolio->Equity);
    case PortfolioVarKind::BarsInPosition:
      const auto &Bars = Portfolio->BarsInPosition;
      if (Portfolio->Position == 0 || !Bars.has_value()) {
        return kNa;
      }
      return static_cast<double>(*Bars);
    }
    return kNa;
  }

  double evalArithmetic(const CompiledExpr &Node) const {
    const double LeftVal = evalNumber(Node.Left);
    const double RightVal = evalNumber(Node.Right);
    if (!std::isfinite(LeftVal) || !std::isfinite(RightVal)) {
      return kNa;
    }
    switch (Node.BinOp) {
    case BinaryOp::Add:
      return normalizeNumber(LeftVal + RightVal);
    case BinaryOp::Sub:
      return normalizeNumber(LeftVal - RightVal);
    case BinaryOp::Mul:
      return normalizeNumber(LeftVal * RightVal);
    case BinaryOp::Div:
      if (RightVal == 0.0) {
        return kNa;
      }
      return normalizeNumber(LeftVal / RightVal);
    default:
      return kNa;
    }
  }

  double evalPureNumberCall(const CompiledExpr &Node) const {
    const double Arg0 = evalNumber(Node.Left);
    if (!std::isfinite(Arg0)) {
      return kNa;
    }
    switch (Node.PureKind) {
    case PureCallKind::Abs:
      return normalizeNumber(std::fabs(Arg0));
    case PureCallKind::Sqrt:
      if (Arg0 < 0.0) {
        return kNa;
      }
      return normalizeNumber(std::sqrt(Arg0));
    case PureCallKind::Min: {
      const double Arg1 = evalNumber(Node.Right);
      if (!std::isfinite(Arg1)) {
        return kNa;
      }
      return (Arg0 < Arg1) ? Arg0 : Arg1;
    }
    case PureCallKind::Max: {
      const double Arg1 = evalNumber(Node.Right);
      if (!std::isfinite(Arg1)) {
        return kNa;
      }
      return (Arg0 > Arg1) ? Arg0 : Arg1;
    }
    case PureCallKind::IsNa:
      return kNa;
    }
    return kNa;
  }

  bool evalLogicalOrCmp(const CompiledExpr &Node) const {
    if (Node.BinOp == BinaryOp::And) {
      if (!evalBool(Node.Left)) {
        return false;
      }
      return evalBool(Node.Right);
    }
    if (Node.BinOp == BinaryOp::Or) {
      if (evalBool(Node.Left)) {
        return true;
      }
      return evalBool(Node.Right);
    }
    if ((Node.BinOp == BinaryOp::Eq || Node.BinOp == BinaryOp::Ne) &&
        Node.BoolOperands) {
      const bool LeftBool = evalBool(Node.Left);
      const bool RightBool = evalBool(Node.Right);
      return (Node.BinOp == BinaryOp::Eq) ? (LeftBool == RightBool)
                                          : (LeftBool != RightBool);
    }

    const double LeftVal = evalNumber(Node.Left);
    const double RightVal = evalNumber(Node.Right);
    if (!std::isfinite(LeftVal) || !std::isfinite(RightVal)) {
      return false;
    }
    switch (Node.BinOp) {
    case BinaryOp::Eq:
      return LeftVal == RightVal;
    case BinaryOp::Ne:
      return LeftVal != RightVal;
    case BinaryOp::Lt:
      return LeftVal < RightVal;
    case BinaryOp::Le:
      return LeftVal <= RightVal;
    case BinaryOp::Gt:
      return LeftVal > RightVal;
    case BinaryOp::Ge:
      return LeftVal >= RightVal;
    default:
      return false;
    }
  }

  const StrategyInstance::Impl &Prog;
  const InstrumentState &Inst;
  const PortfolioSnapshot *Portfolio;
};

std::optional<std::int64_t>
computeOrderLots(const CompiledAction &Action, const Evaluator &Eval,
                 const PortfolioSnapshot &Portfolio, double ClosePrice) {
  if (Action.Unit == SizeUnit::All) {
    if (Portfolio.Position < 1 || Portfolio.Position > kMaxOrderLots) {
      return std::nullopt;
    }
    return Portfolio.Position;
  }

  double RawLots = kNa;
  const double SizeVal = Eval.evalNumber(Action.SizeExprId);
  if (!std::isfinite(SizeVal)) {
    return std::nullopt;
  }
  switch (Action.Unit) {
  case SizeUnit::Lots:
    RawLots = SizeVal;
    break;
  case SizeUnit::PercentPosition:
    RawLots =
        normalizeNumber((static_cast<double>(Portfolio.Position) * SizeVal) /
                        100.0);
    break;
  case SizeUnit::PercentEquity:
  case SizeUnit::PercentCash: {
    if (Portfolio.LotSize <= 0 || !std::isfinite(ClosePrice) ||
        ClosePrice <= 0.0) {
      return std::nullopt;
    }
    const double Base = (Action.Unit == SizeUnit::PercentEquity)
                            ? Portfolio.Equity
                            : Portfolio.Cash;
    if (!std::isfinite(Base)) {
      return std::nullopt;
    }
    const double Denom = ClosePrice * static_cast<double>(Portfolio.LotSize);
    if (!std::isfinite(Denom) || Denom <= 0.0) {
      return std::nullopt;
    }
    RawLots = normalizeNumber(((Base * SizeVal) / 100.0) / Denom);
    break;
  }
  case SizeUnit::All:
    return std::nullopt;
  }

  if (!std::isfinite(RawLots)) {
    return std::nullopt;
  }
  const double Floored = std::floor(RawLots);
  if (!std::isfinite(Floored) || Floored < 1.0 ||
      Floored > static_cast<double>(kMaxOrderLots)) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(Floored);
}

} // namespace

StrategyInstance::StrategyInstance(std::unique_ptr<Impl> State)
    : Pimpl(std::move(State)) {}

StrategyInstance::StrategyInstance(StrategyInstance &&) noexcept = default;
StrategyInstance &
StrategyInstance::operator=(StrategyInstance &&) noexcept = default;
StrategyInstance::~StrategyInstance() = default;

std::size_t StrategyInstance::instrumentCount() const {
  return Pimpl->Instruments.size();
}

std::size_t StrategyInstance::dagNodeCount() const {
  return Pimpl->DagNodes.size();
}

std::size_t StrategyInstance::maxLookback() const {
  return Pimpl->MaxLookback;
}

const std::vector<ParamDecl> &StrategyInstance::params() const {
  return Pimpl->Params;
}

void StrategyInstance::requestCancel() noexcept {
  Pimpl->Cancelled.store(true, std::memory_order_relaxed);
}

bool StrategyInstance::isCancelled() const noexcept {
  return Pimpl->Cancelled.load(std::memory_order_relaxed);
}

StepOutcome StrategyInstance::step(std::size_t InstrumentIdx,
                                   const Bar &CurrentBar,
                                   const PortfolioSnapshot &Portfolio,
                                   bool IsWarmup) {
  StepOutcome Outcome;
  if (isCancelled()) {
    return Outcome;
  }
  if (InstrumentIdx >= Pimpl->Instruments.size()) {
    IntegrityError Err;
    Err.Code = "E-RT01";
    Err.Message = "Индекс инструмента выходит за пределы числа бумаг";
    Outcome.Error = std::move(Err);
    return Outcome;
  }

  InstrumentState &Inst = Pimpl->Instruments[InstrumentIdx];
  if (Inst.LastStep.has_value() && CurrentBar.Step <= *Inst.LastStep) {
    IntegrityError Err;
    Err.Code = "E-RT01";
    Err.Message = "Номер шага бара должен строго возрастать";
    Outcome.Error = std::move(Err);
    return Outcome;
  }

  Inst.Series[0].push(normalizeNumber(CurrentBar.Open));
  Inst.Series[1].push(normalizeNumber(CurrentBar.High));
  Inst.Series[2].push(normalizeNumber(CurrentBar.Low));
  Inst.Series[3].push(normalizeNumber(CurrentBar.Close));
  Inst.Series[4].push(normalizeNumber(CurrentBar.Volume));
  Inst.LastStep = CurrentBar.Step;

  Evaluator DagEval(*Pimpl, Inst, nullptr);
  for (std::size_t I = 0; I < Pimpl->DagNodes.size(); ++I) {
    const DagNodeSpec &Spec = Pimpl->DagNodes[I];
    const double InputA =
        DagEval.evalDagInput(Spec.InputA, Pimpl->DagInputExprA[I]);
    const double InputB =
        DagEval.evalDagInput(Spec.InputB, Pimpl->DagInputExprB[I]);
    internal::updateDagNode(Spec, Inst.DagStates[I], InputA, InputB,
                            CurrentBar);
  }

  if (IsWarmup) {
    return Outcome;
  }

  Evaluator RuleEval(*Pimpl, Inst, &Portfolio);
  const double CloseVal = normalizeNumber(CurrentBar.Close);
  for (const CompiledRule &Rule : Pimpl->Rules) {
    if (!RuleEval.evalBool(Rule.ConditionExprId)) {
      continue;
    }

    if (Rule.Action.Kind == ActionKind::Cancel) {
      RuleIntent Intent;
      Intent.RuleId = Rule.RuleId;
      Intent.RuleName = Rule.Name;
      Intent.Kind = ActionKind::Cancel;
      Intent.OrderKind = OrderType::Market;
      Intent.Lots = 0;
      Intent.RawPrice = kNa;
      Intent.Tif = TimeInForce::Gtc;
      Intent.Because = RuleEval.evalBecause(Rule);
      Outcome.Intents.push_back(std::move(Intent));
      continue;
    }

    const std::optional<std::int64_t> Lots =
        computeOrderLots(Rule.Action, RuleEval, Portfolio, CloseVal);
    if (!Lots.has_value()) {
      continue;
    }

    double RawPrice = kNa;
    if (Rule.Action.OrderKind != OrderType::Market) {
      RawPrice = RuleEval.evalNumber(Rule.Action.PriceExprId);
      if (!std::isfinite(RawPrice) || RawPrice <= 0.0 ||
          RawPrice > kMaxDecimalPrice) {
        continue;
      }
    }

    RuleIntent Intent;
    Intent.RuleId = Rule.RuleId;
    Intent.RuleName = Rule.Name;
    Intent.Kind = Rule.Action.Kind;
    Intent.OrderKind = Rule.Action.OrderKind;
    Intent.Lots = *Lots;
    Intent.RawPrice = RawPrice;
    Intent.Tif = Rule.Action.Tif;
    Intent.Because = RuleEval.evalBecause(Rule);
    Outcome.Intents.push_back(std::move(Intent));
  }

  return Outcome;
}

StepOutcome step(StrategyInstance &Instance, std::size_t InstrumentIdx,
                 const Bar &CurrentBar, const PortfolioSnapshot &Portfolio,
                 bool IsWarmup) {
  return Instance.step(InstrumentIdx, CurrentBar, Portfolio, IsWarmup);
}

} // namespace dsl
