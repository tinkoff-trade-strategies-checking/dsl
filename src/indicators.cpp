#include "internal/indicators.hpp"

#include "dsl/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dsl::internal {
namespace {

double sanitizeFinite(double Value) noexcept {
  return std::isfinite(Value) ? Value : kNa;
}

double reduceExtrema(const RingBuffer &Window, bool FindMax) {
  const std::size_t Size = Window.size();
  if (Size == 0) {
    return kNa;
  }
  double Best = Window.at(Size - 1);
  if (isNaValue(Best)) {
    return kNa;
  }
  for (std::size_t Rem = Size - 1; Rem > 0; --Rem) {
    const double Val = Window.at(Rem - 1);
    if (isNaValue(Val)) {
      return kNa;
    }
    if (FindMax ? (Val > Best) : (Val < Best)) {
      Best = Val;
    }
  }
  return Best;
}

} // namespace

RingBuffer::RingBuffer(std::size_t CapacityValue)
    : Buffer(CapacityValue, kNa) {}

void RingBuffer::push(double Value) {
  if (Buffer.empty()) {
    return;
  }
  Buffer[Head] = Value;
  Head = (Head + 1) % Buffer.size();
  if (Count < Buffer.size()) {
    ++Count;
  }
}

double RingBuffer::at(std::size_t Offset) const {
  if (Offset >= Count) {
    return kNa;
  }
  const std::size_t Cap = Buffer.size();
  return Buffer[(Head + Cap - 1 - Offset) % Cap];
}

void RingBuffer::clear() noexcept {
  Head = 0;
  Count = 0;
}

std::size_t RingBuffer::size() const noexcept { return Count; }

std::size_t RingBuffer::capacity() const noexcept { return Buffer.size(); }

bool RingBuffer::empty() const noexcept { return Count == 0; }

bool RingBuffer::full() const noexcept { return Count == Buffer.size(); }

double RingBuffer::sumOldestToNewest() const {
  if (Count == 0) {
    return 0.0;
  }
  const std::size_t Cap = Buffer.size();
  const std::size_t Start = (Head + Cap - Count) % Cap;
  double Sum = 0.0;
  for (std::size_t Idx = 0; Idx < Count; ++Idx) {
    Sum += Buffer[(Start + Idx) % Cap];
  }
  return Sum;
}

double RingBuffer::stdevTwoPassOldestToNewest() const {
  if (Count == 0) {
    return kNa;
  }
  if (Count == 1) {
    return std::isfinite(at(0)) ? 0.0 : kNa;
  }
  const std::size_t Cap = Buffer.size();
  const std::size_t Start = (Head + Cap - Count) % Cap;
  double Sum = 0.0;
  for (std::size_t Idx = 0; Idx < Count; ++Idx) {
    Sum += Buffer[(Start + Idx) % Cap];
  }
  const double Mean = Sum / static_cast<double>(Count);
  if (!std::isfinite(Mean)) {
    return kNa;
  }
  double SqSum = 0.0;
  for (std::size_t Idx = 0; Idx < Count; ++Idx) {
    const double Diff = Buffer[(Start + Idx) % Cap] - Mean;
    SqSum += Diff * Diff;
  }
  const double Result = std::sqrt(SqSum / static_cast<double>(Count));
  return sanitizeFinite(Result);
}

double RingBuffer::maxOldestToNewest() const {
  return reduceExtrema(*this, true);
}

double RingBuffer::minOldestToNewest() const {
  return reduceExtrema(*this, false);
}

namespace {

double computeRsi(double AvgGain, double AvgLoss) noexcept {
  if (!std::isfinite(AvgGain) || !std::isfinite(AvgLoss)) {
    return kNa;
  }
  if (AvgGain == 0.0 && AvgLoss == 0.0) {
    return 50.0;
  }
  if (AvgLoss == 0.0) {
    return 100.0;
  }
  return 100.0 * AvgGain / (AvgGain + AvgLoss);
}

double updateWindowStat(const DagNodeSpec &Spec, DagNodeState &State,
                        double InputA) {
  if (Spec.Period == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  State.InputWindow.push(InputA);
  if (State.InputWindow.size() < Spec.Period) {
    return kNa;
  }
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
    return State.InputWindow.sumOldestToNewest() /
           static_cast<double>(Spec.Period);
  case DagNodeKind::Stdev:
    return State.InputWindow.stdevTwoPassOldestToNewest();
  case DagNodeKind::Highest:
    return State.InputWindow.maxOldestToNewest();
  case DagNodeKind::Lowest:
    return State.InputWindow.minOldestToNewest();
  case DagNodeKind::Ema:
  case DagNodeKind::Rsi:
  case DagNodeKind::Atr:
  case DagNodeKind::Change:
  case DagNodeKind::Crossover:
  case DagNodeKind::Crossunder:
    return kNa;
  }
  return kNa;
}

double updateEma(std::size_t Period, DagNodeState &State, double InputA) {
  if (Period == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  if (!State.EmaInitialized) {
    State.InputWindow.push(InputA);
    if (State.InputWindow.size() < Period) {
      return kNa;
    }
    State.EmaPrev =
        State.InputWindow.sumOldestToNewest() / static_cast<double>(Period);
    State.EmaInitialized = true;
    State.InputWindow.clear();
    return State.EmaPrev;
  }
  const double Alpha = 2.0 / (static_cast<double>(Period) + 1.0);
  State.EmaPrev = Alpha * InputA + (1.0 - Alpha) * State.EmaPrev;
  return State.EmaPrev;
}

double updateRsi(std::size_t Period, DagNodeState &State, double InputA) {
  if (Period == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  if (!State.HasPrevInput) {
    State.PrevInput = InputA;
    State.HasPrevInput = true;
    return kNa;
  }
  const double Delta = InputA - State.PrevInput;
  State.PrevInput = InputA;
  const double Gain = Delta > 0.0 ? Delta : 0.0;
  const double Loss = Delta < 0.0 ? -Delta : 0.0;
  ++State.RsiCount;
  const auto PeriodD = static_cast<double>(Period);
  if (State.RsiCount < Period) {
    State.GainSum += Gain;
    State.LossSum += Loss;
    return kNa;
  }
  if (State.RsiCount == Period) {
    State.GainSum += Gain;
    State.LossSum += Loss;
    State.AvgGain = State.GainSum / PeriodD;
    State.AvgLoss = State.LossSum / PeriodD;
  } else {
    const auto PrevWeight = static_cast<double>(Period - 1);
    State.AvgGain = (State.AvgGain * PrevWeight + Gain) / PeriodD;
    State.AvgLoss = (State.AvgLoss * PrevWeight + Loss) / PeriodD;
  }
  return computeRsi(State.AvgGain, State.AvgLoss);
}

double updateAtr(std::size_t Period, DagNodeState &State,
                 const Bar &CurrentBar) {
  if (Period == 0 || !std::isfinite(CurrentBar.High) ||
      !std::isfinite(CurrentBar.Low) || !std::isfinite(CurrentBar.Close)) {
    return kNa;
  }
  double Tr = CurrentBar.High - CurrentBar.Low;
  if (State.HasPrevClose) {
    const double HighClose = std::abs(CurrentBar.High - State.PrevClose);
    const double LowClose = std::abs(CurrentBar.Low - State.PrevClose);
    Tr = std::max({Tr, HighClose, LowClose});
  }
  if (!std::isfinite(Tr)) {
    return kNa;
  }
  State.PrevClose = CurrentBar.Close;
  State.HasPrevClose = true;
  ++State.AtrCount;
  const auto PeriodD = static_cast<double>(Period);
  if (State.AtrCount < Period) {
    State.TrSum += Tr;
    return kNa;
  }
  if (State.AtrCount == Period) {
    State.TrSum += Tr;
    State.AvgTr = State.TrSum / PeriodD;
  } else {
    const auto PrevWeight = static_cast<double>(Period - 1);
    State.AvgTr = (State.AvgTr * PrevWeight + Tr) / PeriodD;
  }
  return State.AvgTr;
}

double updateChange(std::size_t Period, DagNodeState &State, double InputA) {
  if (Period == 0) {
    return kNa;
  }
  const double Current = sanitizeFinite(InputA);
  State.InputWindow.push(Current);
  if (State.InputWindow.size() <= Period) {
    return kNa;
  }
  const double Past = State.InputWindow.at(Period);
  if (!std::isfinite(Current) || !std::isfinite(Past)) {
    return kNa;
  }
  return Current - Past;
}

double updateCross(DagNodeState &State, double InputA, double InputB,
                   bool IsCrossover) {
  const double CurA = sanitizeFinite(InputA);
  const double CurB = sanitizeFinite(InputB);
  bool Crossed = false;
  if (State.HasPrevCross && std::isfinite(State.PrevA) &&
      std::isfinite(State.PrevB) && std::isfinite(CurA) &&
      std::isfinite(CurB)) {
    Crossed = IsCrossover ? (State.PrevA <= State.PrevB && CurA > CurB)
                          : (State.PrevA >= State.PrevB && CurA < CurB);
  }
  State.PrevA = CurA;
  State.PrevB = CurB;
  State.HasPrevCross = true;
  return Crossed ? 1.0 : 0.0;
}

} // namespace

DagNodeState makeDagNodeState(const DagNodeSpec &Spec) {
  DagNodeState State;
  State.Output = RingBuffer(Spec.OutputCapacity);
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
  case DagNodeKind::Ema:
  case DagNodeKind::Stdev:
  case DagNodeKind::Highest:
  case DagNodeKind::Lowest:
    State.InputWindow = RingBuffer(Spec.Period);
    break;
  case DagNodeKind::Change:
    State.InputWindow = RingBuffer(Spec.Period + 1);
    break;
  case DagNodeKind::Rsi:
  case DagNodeKind::Atr:
  case DagNodeKind::Crossover:
  case DagNodeKind::Crossunder:
    break;
  }
  return State;
}

double updateDagNode(const DagNodeSpec &Spec, DagNodeState &State,
                     double InputA, double InputB, const Bar &CurrentBar) {
  double Result = kNa;
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
  case DagNodeKind::Stdev:
  case DagNodeKind::Highest:
  case DagNodeKind::Lowest:
    Result = updateWindowStat(Spec, State, InputA);
    break;
  case DagNodeKind::Ema:
    Result = updateEma(Spec.Period, State, InputA);
    break;
  case DagNodeKind::Rsi:
    Result = updateRsi(Spec.Period, State, InputA);
    break;
  case DagNodeKind::Atr:
    Result = updateAtr(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::Change:
    Result = updateChange(Spec.Period, State, InputA);
    break;
  case DagNodeKind::Crossover:
    Result = updateCross(State, InputA, InputB, true);
    break;
  case DagNodeKind::Crossunder:
    Result = updateCross(State, InputA, InputB, false);
    break;
  }
  const double SafeResult = sanitizeFinite(Result);
  State.Output.push(SafeResult);
  return SafeResult;
}

} // namespace dsl::internal
