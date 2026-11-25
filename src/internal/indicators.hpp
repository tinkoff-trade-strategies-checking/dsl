#pragma once

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace dsl::internal {

/// Fixed-capacity ring buffer with O(1) push and deterministic
/// oldest-to-newest reductions (INV-4).
class RingBuffer {
public:
  RingBuffer() = default;
  explicit RingBuffer(std::size_t CapacityValue);

  /// Overwrites the oldest slot once full. No-op when capacity is zero.
  void push(double Value);

  /// Returns the value `Offset` bars ago (`0` is newest), or `kNa` when
  /// `Offset >= size()`.
  double at(std::size_t Offset) const;

  void clear() noexcept;
  std::size_t size() const noexcept;
  std::size_t capacity() const noexcept;
  bool empty() const noexcept;
  bool full() const noexcept;

  /// Sums stored elements strictly from oldest to newest.
  double sumOldestToNewest() const;

  /// Sums the newest `Len` stored elements strictly from oldest to newest.
  double sumLastOldestToNewest(std::size_t Len) const;

  /// Linearly weighted moving average (weights 1..Count) oldest to newest.
  double wmaOldestToNewest() const;

  /// Mean absolute deviation around `Mean` strictly from oldest to newest.
  double meanAbsDevOldestToNewest(double Mean) const;

  /// Two-pass population standard deviation strictly from oldest to newest.
  double stdevTwoPassOldestToNewest() const;

  /// Maximum element across stored slots from oldest to newest.
  double maxOldestToNewest() const;

  /// Minimum element across stored slots from oldest to newest.
  double minOldestToNewest() const;

private:
  std::vector<double> Buffer;
  std::size_t Head = 0;
  std::size_t Count = 0;
};

enum class DagNodeKind {
  Sma,
  Ema,
  Wma,
  Rsi,
  Atr,
  Stdev,
  Highest,
  Lowest,
  Change,
  Crossover,
  Crossunder,
  BbMid,
  BbUpper,
  BbLower,
  Vwap,
  Macd,
  MacdSignal,
  MacdHist,
  StochK,
  StochD,
  StochRsi,
  Adx,
  PlusDi,
  MinusDi,
  Cci,
  WillR,
  Roc,
  UltOsc,
  BullPower,
  BearPower,
  Obv,
  Mfi
};

enum class SeriesKind { Open, High, Low, Close, Volume };

enum class DagInputKind { None, Series, DagNode, Constant, ExprAst };

/// Describes a single argument passed into a DAG node at step time.
struct DagInputDesc {
  DagInputKind Kind = DagInputKind::None;
  SeriesKind Series = SeriesKind::Close;
  std::size_t NodeIndex = 0;
  std::size_t Offset = 0;
  double ConstantValue = 0.0;
  const Expr *AstExpr = nullptr;
};

/// Topologically ordered DAG node definition produced at compile time.
struct DagNodeSpec {
  DagNodeKind Kind = DagNodeKind::Sma;
  std::string CanonicalKey;
  std::size_t Period = 0;
  std::size_t Period2 = 0;
  std::size_t Period3 = 0;
  DagInputDesc InputA;
  DagInputDesc InputB;
  std::size_t OutputCapacity = 1;
};

/// Reusable EMA state for composite indicators (MACD, signal).
struct EmaTracker {
  RingBuffer InitWindow;
  double Prev = kNa;
  bool Initialized = false;
};

/// Per-instrument mutable state for one DAG node.
struct DagNodeState {
  RingBuffer Output;
  RingBuffer InputWindow;
  RingBuffer AuxWindow;
  RingBuffer AuxWindow2;
  double EmaPrev = kNa;
  bool EmaInitialized = false;
  EmaTracker EmaFast;
  EmaTracker EmaSlow;
  EmaTracker EmaSignal;
  double AvgGain = 0.0;
  double AvgLoss = 0.0;
  double GainSum = 0.0;
  double LossSum = 0.0;
  double PrevInput = kNa;
  bool HasPrevInput = false;
  std::size_t RsiCount = 0;
  double AvgTr = 0.0;
  double TrSum = 0.0;
  double PrevHigh = kNa;
  double PrevLow = kNa;
  double PrevClose = kNa;
  bool HasPrevClose = false;
  std::size_t AtrCount = 0;
  double AvgPlusDm = 0.0;
  double AvgMinusDm = 0.0;
  double PlusDmSum = 0.0;
  double MinusDmSum = 0.0;
  std::size_t DmCount = 0;
  double AvgAdx = 0.0;
  double AdxSum = 0.0;
  std::size_t AdxCount = 0;
  double ObvValue = 0.0;
  double PrevA = kNa;
  double PrevB = kNa;
  bool HasPrevCross = false;
};

/// Allocates buffers for `Spec` (`Output` and working windows if needed).
DagNodeState makeDagNodeState(const DagNodeSpec &Spec);

/// Updates one DAG node for the current trading bar, preserving internal
/// state across `na` inputs for stateful indicators and recording `Output`.
double updateDagNode(const DagNodeSpec &Spec, DagNodeState &State,
                     double InputA, double InputB, const Bar &CurrentBar);

} // namespace dsl::internal
