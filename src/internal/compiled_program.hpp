#pragma once

#include "dsl/ast.hpp"
#include "dsl/interpreter.hpp"
#include "dsl/types.hpp"
#include "internal/indicators.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace dsl::internal {

inline constexpr std::size_t kInvalidExprId =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t kSeriesCount = 5;

enum class PortfolioVarKind {
  Position,
  EntryPrice,
  Cash,
  Equity,
  BarsInPosition
};

enum class PureCallKind { Abs, Min, Max, Sqrt, IsNa };

enum class CompiledExprOp {
  NumberLit,
  BoolLit,
  Series,
  DagNode,
  PortfolioVar,
  Neg,
  Not,
  Binary,
  PureCall
};

struct CompiledExpr {
  CompiledExprOp Op = CompiledExprOp::NumberLit;
  double NumberVal = 0.0;
  bool BoolVal = false;
  std::size_t Index = 0;
  std::size_t Offset = 0;
  PortfolioVarKind VarKind = PortfolioVarKind::Position;
  BinaryOp BinOp = BinaryOp::Add;
  PureCallKind PureKind = PureCallKind::Abs;
  std::size_t Left = kInvalidExprId;
  std::size_t Right = kInvalidExprId;
  bool BoolOperands = false;
};

struct CompiledBecausePart {
  bool IsExpr = false;
  bool IsBoolExpr = false;
  std::string LiteralText;
  std::size_t ExprId = kInvalidExprId;
};

struct CompiledAction {
  ActionKind Kind = ActionKind::Buy;
  SizeUnit Unit = SizeUnit::Lots;
  std::size_t SizeExprId = kInvalidExprId;
  OrderType OrderKind = OrderType::Market;
  std::size_t PriceExprId = kInvalidExprId;
  TimeInForce Tif = TimeInForce::Gtc;
};

struct CompiledRule {
  std::size_t RuleId = 0;
  std::string Name;
  std::size_t ConditionExprId = kInvalidExprId;
  CompiledAction Action;
  std::vector<CompiledBecausePart> BecauseParts;
};

struct InstrumentState {
  std::array<RingBuffer, kSeriesCount> Series;
  std::vector<DagNodeState> DagStates;
  std::optional<std::int64_t> LastStep;
};

} // namespace dsl::internal

namespace dsl {

struct StrategyInstance::Impl {
  Program OwnedAst;
  std::vector<ParamDecl> Params;
  std::array<std::size_t, internal::kSeriesCount> SeriesCaps{1, 1, 1, 1, 1};
  std::vector<internal::DagNodeSpec> DagNodes;
  std::vector<std::size_t> DagInputExprA;
  std::vector<std::size_t> DagInputExprB;
  std::vector<internal::CompiledExpr> ExprPool;
  std::vector<internal::CompiledRule> Rules;
  std::vector<internal::InstrumentState> Instruments;
  std::size_t MaxLookback = 0;
  std::atomic<bool> Cancelled{false};
};

} // namespace dsl
