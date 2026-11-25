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

bool isValidOhlc(const Bar &CurrentBar) noexcept {
  return std::isfinite(CurrentBar.High) && std::isfinite(CurrentBar.Low) &&
         std::isfinite(CurrentBar.Close);
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
  return sumLastOldestToNewest(Count);
}

double RingBuffer::sumLastOldestToNewest(std::size_t Len) const {
  if (Len == 0 || Count == 0) {
    return 0.0;
  }
  const std::size_t Used = std::min(Len, Count);
  const std::size_t Cap = Buffer.size();
  const std::size_t Start = (Head + Cap - Used) % Cap;
  double Sum = 0.0;
  for (std::size_t Idx = 0; Idx < Used; ++Idx) {
    Sum += Buffer[(Start + Idx) % Cap];
  }
  return Sum;
}

double RingBuffer::wmaOldestToNewest() const {
  if (Count == 0) {
    return kNa;
  }
  const std::size_t Cap = Buffer.size();
  const std::size_t Start = (Head + Cap - Count) % Cap;
  double WeightedSum = 0.0;
  for (std::size_t Idx = 0; Idx < Count; ++Idx) {
    const auto Weight = static_cast<double>(Idx + 1);
    WeightedSum += Weight * Buffer[(Start + Idx) % Cap];
  }
  const auto CountD = static_cast<double>(Count);
  const double Denom = CountD * (CountD + 1.0) / 2.0;
  return sanitizeFinite(WeightedSum / Denom);
}

double RingBuffer::meanAbsDevOldestToNewest(double Mean) const {
  if (Count == 0 || !std::isfinite(Mean)) {
    return kNa;
  }
  const std::size_t Cap = Buffer.size();
  const std::size_t Start = (Head + Cap - Count) % Cap;
  double AbsSum = 0.0;
  for (std::size_t Idx = 0; Idx < Count; ++Idx) {
    AbsSum += std::abs(Buffer[(Start + Idx) % Cap] - Mean);
  }
  return sanitizeFinite(AbsSum / static_cast<double>(Count));
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
  const double Sum = AvgGain + AvgLoss;
  const double Numerator = 100.0 * AvgGain;
  if (std::isfinite(Sum) && std::isfinite(Numerator)) {
    return Numerator / Sum;
  }
  if (AvgGain >= AvgLoss) {
    return 100.0 / (1.0 + AvgLoss / AvgGain);
  }
  const double Ratio = AvgGain / AvgLoss;
  return 100.0 * Ratio / (1.0 + Ratio);
}

double stepEmaTracker(EmaTracker &Tracker, std::size_t Period, double Input) {
  if (Period == 0 || !std::isfinite(Input)) {
    return kNa;
  }
  if (!Tracker.Initialized) {
    Tracker.InitWindow.push(Input);
    if (Tracker.InitWindow.size() < Period) {
      return kNa;
    }
    const double InitVal =
        Tracker.InitWindow.sumOldestToNewest() / static_cast<double>(Period);
    if (!std::isfinite(InitVal)) {
      return kNa;
    }
    Tracker.Prev = InitVal;
    Tracker.Initialized = true;
    Tracker.InitWindow.clear();
    return Tracker.Prev;
  }
  const double Alpha = 2.0 / (static_cast<double>(Period) + 1.0);
  const double NextEma = Alpha * Input + (1.0 - Alpha) * Tracker.Prev;
  if (!std::isfinite(NextEma)) {
    return kNa;
  }
  Tracker.Prev = NextEma;
  return Tracker.Prev;
}

double updateWindowStat(const DagNodeSpec &Spec, DagNodeState &State,
                        double InputA, double InputB) {
  if (Spec.Period == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  State.InputWindow.push(InputA);
  if (State.InputWindow.size() < Spec.Period) {
    return kNa;
  }
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
  case DagNodeKind::BbMid:
    return State.InputWindow.sumOldestToNewest() /
           static_cast<double>(Spec.Period);
  case DagNodeKind::Wma:
    return State.InputWindow.wmaOldestToNewest();
  case DagNodeKind::Stdev:
    return State.InputWindow.stdevTwoPassOldestToNewest();
  case DagNodeKind::Highest:
    return State.InputWindow.maxOldestToNewest();
  case DagNodeKind::Lowest:
    return State.InputWindow.minOldestToNewest();
  case DagNodeKind::BbUpper:
  case DagNodeKind::BbLower: {
    if (!std::isfinite(InputB)) {
      return kNa;
    }
    const double Mean = State.InputWindow.sumOldestToNewest() /
                        static_cast<double>(Spec.Period);
    const double Sd = State.InputWindow.stdevTwoPassOldestToNewest();
    if (!std::isfinite(Mean) || !std::isfinite(Sd)) {
      return kNa;
    }
    return Spec.Kind == DagNodeKind::BbUpper ? (Mean + InputB * Sd)
                                             : (Mean - InputB * Sd);
  }
  default:
    return kNa;
  }
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
    const double InitVal =
        State.InputWindow.sumOldestToNewest() / static_cast<double>(Period);
    if (!std::isfinite(InitVal)) {
      return kNa;
    }
    State.EmaPrev = InitVal;
    State.EmaInitialized = true;
    State.InputWindow.clear();
    return State.EmaPrev;
  }
  const double Alpha = 2.0 / (static_cast<double>(Period) + 1.0);
  const double NextEma = Alpha * InputA + (1.0 - Alpha) * State.EmaPrev;
  if (!std::isfinite(NextEma)) {
    return kNa;
  }
  State.EmaPrev = NextEma;
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
  if (!std::isfinite(Delta)) {
    return kNa;
  }
  const double Gain = Delta > 0.0 ? Delta : 0.0;
  const double Loss = Delta < 0.0 ? -Delta : 0.0;
  const std::size_t NextCount = State.RsiCount + 1;
  const auto PeriodD = static_cast<double>(Period);
  if (NextCount < Period) {
    const double NextGainSum = State.GainSum + Gain;
    const double NextLossSum = State.LossSum + Loss;
    if (!std::isfinite(NextGainSum) || !std::isfinite(NextLossSum)) {
      return kNa;
    }
    State.PrevInput = InputA;
    State.RsiCount = NextCount;
    State.GainSum = NextGainSum;
    State.LossSum = NextLossSum;
    return kNa;
  }
  if (NextCount == Period) {
    const double NextGainSum = State.GainSum + Gain;
    const double NextLossSum = State.LossSum + Loss;
    const double NextAvgGain = NextGainSum / PeriodD;
    const double NextAvgLoss = NextLossSum / PeriodD;
    if (!std::isfinite(NextGainSum) || !std::isfinite(NextLossSum) ||
        !std::isfinite(NextAvgGain) || !std::isfinite(NextAvgLoss)) {
      return kNa;
    }
    State.PrevInput = InputA;
    State.RsiCount = NextCount;
    State.GainSum = NextGainSum;
    State.LossSum = NextLossSum;
    State.AvgGain = NextAvgGain;
    State.AvgLoss = NextAvgLoss;
  } else {
    const auto PrevWeight = static_cast<double>(Period - 1);
    const double NextAvgGain = (State.AvgGain * PrevWeight + Gain) / PeriodD;
    const double NextAvgLoss = (State.AvgLoss * PrevWeight + Loss) / PeriodD;
    if (!std::isfinite(NextAvgGain) || !std::isfinite(NextAvgLoss)) {
      return kNa;
    }
    State.PrevInput = InputA;
    State.RsiCount = NextCount;
    State.AvgGain = NextAvgGain;
    State.AvgLoss = NextAvgLoss;
  }
  return computeRsi(State.AvgGain, State.AvgLoss);
}

double updateAtr(std::size_t Period, DagNodeState &State,
                 const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar)) {
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
  const std::size_t NextCount = State.AtrCount + 1;
  const auto PeriodD = static_cast<double>(Period);
  if (NextCount < Period) {
    const double NextTrSum = State.TrSum + Tr;
    if (!std::isfinite(NextTrSum)) {
      return kNa;
    }
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    State.AtrCount = NextCount;
    State.TrSum = NextTrSum;
    return kNa;
  }
  if (NextCount == Period) {
    const double NextTrSum = State.TrSum + Tr;
    const double NextAvgTr = NextTrSum / PeriodD;
    if (!std::isfinite(NextTrSum) || !std::isfinite(NextAvgTr)) {
      return kNa;
    }
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    State.AtrCount = NextCount;
    State.TrSum = NextTrSum;
    State.AvgTr = NextAvgTr;
  } else {
    const auto PrevWeight = static_cast<double>(Period - 1);
    const double NextAvgTr = (State.AvgTr * PrevWeight + Tr) / PeriodD;
    if (!std::isfinite(NextAvgTr)) {
      return kNa;
    }
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    State.AtrCount = NextCount;
    State.AvgTr = NextAvgTr;
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

double updateRoc(std::size_t Period, DagNodeState &State, double InputA) {
  if (Period == 0) {
    return kNa;
  }
  const double Current = sanitizeFinite(InputA);
  State.InputWindow.push(Current);
  if (State.InputWindow.size() <= Period) {
    return kNa;
  }
  const double Past = State.InputWindow.at(Period);
  if (!std::isfinite(Current) || !std::isfinite(Past) || Past == 0.0) {
    return kNa;
  }
  return 100.0 * (Current - Past) / Past;
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

double updateVwap(std::size_t Period, DagNodeState &State,
                  const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar) ||
      !std::isfinite(CurrentBar.Volume) || CurrentBar.Volume < 0.0) {
    return kNa;
  }
  const double Tp = (CurrentBar.High + CurrentBar.Low + CurrentBar.Close) / 3.0;
  const double Pv = Tp * CurrentBar.Volume;
  if (!std::isfinite(Tp) || !std::isfinite(Pv)) {
    return kNa;
  }
  State.InputWindow.push(Pv);
  State.AuxWindow.push(CurrentBar.Volume);
  if (State.InputWindow.size() < Period) {
    return kNa;
  }
  const double SumPv = State.InputWindow.sumOldestToNewest();
  const double SumVol = State.AuxWindow.sumOldestToNewest();
  if (!std::isfinite(SumPv) || !std::isfinite(SumVol) || SumVol <= 0.0) {
    return kNa;
  }
  return SumPv / SumVol;
}

double updateMacdFamily(const DagNodeSpec &Spec, DagNodeState &State,
                        double InputA) {
  if (Spec.Period == 0 || Spec.Period2 == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  const double FastVal = stepEmaTracker(State.EmaFast, Spec.Period, InputA);
  const double SlowVal = stepEmaTracker(State.EmaSlow, Spec.Period2, InputA);
  if (!std::isfinite(FastVal) || !std::isfinite(SlowVal)) {
    return kNa;
  }
  const double MacdVal = FastVal - SlowVal;
  if (!std::isfinite(MacdVal)) {
    return kNa;
  }
  if (Spec.Kind == DagNodeKind::Macd) {
    return MacdVal;
  }
  const double SigVal = stepEmaTracker(State.EmaSignal, Spec.Period3, MacdVal);
  if (!std::isfinite(SigVal)) {
    return kNa;
  }
  if (Spec.Kind == DagNodeKind::MacdSignal) {
    return SigVal;
  }
  return MacdVal - SigVal;
}

double updateStoch(const DagNodeSpec &Spec, DagNodeState &State,
                   const Bar &CurrentBar) {
  if (Spec.Period == 0 || !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  State.InputWindow.push(CurrentBar.High);
  State.AuxWindow.push(CurrentBar.Low);
  if (State.InputWindow.size() < Spec.Period) {
    return kNa;
  }
  const double HighestHigh = State.InputWindow.maxOldestToNewest();
  const double LowestLow = State.AuxWindow.minOldestToNewest();
  const double Span = HighestHigh - LowestLow;
  if (!std::isfinite(Span)) {
    return kNa;
  }
  const double StochKVal =
      (Span == 0.0)
          ? 50.0
          : sanitizeFinite(100.0 * (CurrentBar.Close - LowestLow) / Span);
  if (Spec.Kind == DagNodeKind::StochK) {
    return StochKVal;
  }
  if (!std::isfinite(StochKVal) || Spec.Period2 == 0) {
    return kNa;
  }
  State.AuxWindow2.push(StochKVal);
  if (State.AuxWindow2.size() < Spec.Period2) {
    return kNa;
  }
  return State.AuxWindow2.sumOldestToNewest() /
         static_cast<double>(Spec.Period2);
}

double updateStochRsi(std::size_t Period, DagNodeState &State,
                      const Bar &CurrentBar) {
  if (Period == 0 || !std::isfinite(CurrentBar.Close)) {
    return kNa;
  }
  const double RsiVal = updateRsi(Period, State, CurrentBar.Close);
  if (!std::isfinite(RsiVal)) {
    return kNa;
  }
  State.AuxWindow.push(RsiVal);
  if (State.AuxWindow.size() < Period) {
    return kNa;
  }
  const double Hi = State.AuxWindow.maxOldestToNewest();
  const double Lo = State.AuxWindow.minOldestToNewest();
  const double Span = Hi - Lo;
  if (!std::isfinite(Span)) {
    return kNa;
  }
  if (Span == 0.0) {
    return 50.0;
  }
  return 100.0 * (RsiVal - Lo) / Span;
}

bool stepDirectional(std::size_t Period, DagNodeState &State,
                     const Bar &CurrentBar, double &OutPlusDi,
                     double &OutMinusDi) {
  if (Period == 0 || !isValidOhlc(CurrentBar)) {
    return false;
  }
  if (!State.HasPrevClose) {
    State.PrevHigh = CurrentBar.High;
    State.PrevLow = CurrentBar.Low;
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    return false;
  }
  const double UpMove = CurrentBar.High - State.PrevHigh;
  const double DownMove = State.PrevLow - CurrentBar.Low;
  const double HighLow = CurrentBar.High - CurrentBar.Low;
  const double HighClose = std::abs(CurrentBar.High - State.PrevClose);
  const double LowClose = std::abs(CurrentBar.Low - State.PrevClose);
  const double Tr = std::max({HighLow, HighClose, LowClose});
  if (!std::isfinite(UpMove) || !std::isfinite(DownMove) ||
      !std::isfinite(Tr)) {
    return false;
  }
  const double PlusDm = (UpMove > DownMove && UpMove > 0.0) ? UpMove : 0.0;
  const double MinusDm = (DownMove > UpMove && DownMove > 0.0) ? DownMove : 0.0;
  const std::size_t NextCount = State.DmCount + 1;
  const auto PeriodD = static_cast<double>(Period);
  if (NextCount < Period) {
    const double NextTrSum = State.TrSum + Tr;
    const double NextPlusSum = State.PlusDmSum + PlusDm;
    const double NextMinusSum = State.MinusDmSum + MinusDm;
    if (!std::isfinite(NextTrSum) || !std::isfinite(NextPlusSum) ||
        !std::isfinite(NextMinusSum)) {
      return false;
    }
    State.PrevHigh = CurrentBar.High;
    State.PrevLow = CurrentBar.Low;
    State.PrevClose = CurrentBar.Close;
    State.DmCount = NextCount;
    State.TrSum = NextTrSum;
    State.PlusDmSum = NextPlusSum;
    State.MinusDmSum = NextMinusSum;
    return false;
  }
  if (NextCount == Period) {
    const double NextTrSum = State.TrSum + Tr;
    const double NextPlusSum = State.PlusDmSum + PlusDm;
    const double NextMinusSum = State.MinusDmSum + MinusDm;
    const double NextAvgTr = NextTrSum / PeriodD;
    const double NextAvgPlus = NextPlusSum / PeriodD;
    const double NextAvgMinus = NextMinusSum / PeriodD;
    if (!std::isfinite(NextTrSum) || !std::isfinite(NextPlusSum) ||
        !std::isfinite(NextMinusSum) || !std::isfinite(NextAvgTr) ||
        !std::isfinite(NextAvgPlus) || !std::isfinite(NextAvgMinus)) {
      return false;
    }
    State.PrevHigh = CurrentBar.High;
    State.PrevLow = CurrentBar.Low;
    State.PrevClose = CurrentBar.Close;
    State.DmCount = NextCount;
    State.TrSum = NextTrSum;
    State.PlusDmSum = NextPlusSum;
    State.MinusDmSum = NextMinusSum;
    State.AvgTr = NextAvgTr;
    State.AvgPlusDm = NextAvgPlus;
    State.AvgMinusDm = NextAvgMinus;
  } else {
    const auto PrevWeight = static_cast<double>(Period - 1);
    const double NextAvgTr = (State.AvgTr * PrevWeight + Tr) / PeriodD;
    const double NextAvgPlus =
        (State.AvgPlusDm * PrevWeight + PlusDm) / PeriodD;
    const double NextAvgMinus =
        (State.AvgMinusDm * PrevWeight + MinusDm) / PeriodD;
    if (!std::isfinite(NextAvgTr) || !std::isfinite(NextAvgPlus) ||
        !std::isfinite(NextAvgMinus)) {
      return false;
    }
    State.PrevHigh = CurrentBar.High;
    State.PrevLow = CurrentBar.Low;
    State.PrevClose = CurrentBar.Close;
    State.DmCount = NextCount;
    State.AvgTr = NextAvgTr;
    State.AvgPlusDm = NextAvgPlus;
    State.AvgMinusDm = NextAvgMinus;
  }
  if (State.AvgTr == 0.0) {
    OutPlusDi = 0.0;
    OutMinusDi = 0.0;
  } else {
    OutPlusDi = sanitizeFinite(100.0 * State.AvgPlusDm / State.AvgTr);
    OutMinusDi = sanitizeFinite(100.0 * State.AvgMinusDm / State.AvgTr);
  }
  return std::isfinite(OutPlusDi) && std::isfinite(OutMinusDi);
}

double updateDirectionalFamily(const DagNodeSpec &Spec, DagNodeState &State,
                               const Bar &CurrentBar) {
  double PlusDiVal = kNa;
  double MinusDiVal = kNa;
  if (!stepDirectional(Spec.Period, State, CurrentBar, PlusDiVal, MinusDiVal)) {
    return kNa;
  }
  if (Spec.Kind == DagNodeKind::PlusDi) {
    return PlusDiVal;
  }
  if (Spec.Kind == DagNodeKind::MinusDi) {
    return MinusDiVal;
  }
  const double DiSum = PlusDiVal + MinusDiVal;
  const double Dx =
      (DiSum == 0.0)
          ? 0.0
          : sanitizeFinite(100.0 * std::abs(PlusDiVal - MinusDiVal) / DiSum);
  if (!std::isfinite(Dx)) {
    return kNa;
  }
  const std::size_t NextCount = State.AdxCount + 1;
  const auto PeriodD = static_cast<double>(Spec.Period);
  if (NextCount < Spec.Period) {
    const double NextAdxSum = State.AdxSum + Dx;
    if (!std::isfinite(NextAdxSum)) {
      return kNa;
    }
    State.AdxSum = NextAdxSum;
    State.AdxCount = NextCount;
    return kNa;
  }
  if (NextCount == Spec.Period) {
    const double NextAdxSum = State.AdxSum + Dx;
    const double NextAvgAdx = NextAdxSum / PeriodD;
    if (!std::isfinite(NextAdxSum) || !std::isfinite(NextAvgAdx)) {
      return kNa;
    }
    State.AdxSum = NextAdxSum;
    State.AvgAdx = NextAvgAdx;
    State.AdxCount = NextCount;
    return State.AvgAdx;
  }
  const auto PrevWeight = static_cast<double>(Spec.Period - 1);
  const double NextAvgAdx = (State.AvgAdx * PrevWeight + Dx) / PeriodD;
  if (!std::isfinite(NextAvgAdx)) {
    return kNa;
  }
  State.AvgAdx = NextAvgAdx;
  State.AdxCount = NextCount;
  return State.AvgAdx;
}

double updateCci(std::size_t Period, DagNodeState &State,
                 const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  const double Tp = (CurrentBar.High + CurrentBar.Low + CurrentBar.Close) / 3.0;
  if (!std::isfinite(Tp)) {
    return kNa;
  }
  State.InputWindow.push(Tp);
  if (State.InputWindow.size() < Period) {
    return kNa;
  }
  const double Mean =
      State.InputWindow.sumOldestToNewest() / static_cast<double>(Period);
  const double Mad = State.InputWindow.meanAbsDevOldestToNewest(Mean);
  if (!std::isfinite(Mean) || !std::isfinite(Mad)) {
    return kNa;
  }
  if (Mad == 0.0) {
    return 0.0;
  }
  return (Tp - Mean) / (0.015 * Mad);
}

double updateWillR(std::size_t Period, DagNodeState &State,
                   const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  State.InputWindow.push(CurrentBar.High);
  State.AuxWindow.push(CurrentBar.Low);
  if (State.InputWindow.size() < Period) {
    return kNa;
  }
  const double HighestHigh = State.InputWindow.maxOldestToNewest();
  const double LowestLow = State.AuxWindow.minOldestToNewest();
  const double Span = HighestHigh - LowestLow;
  if (!std::isfinite(Span)) {
    return kNa;
  }
  if (Span == 0.0) {
    return -50.0;
  }
  return -100.0 * (HighestHigh - CurrentBar.Close) / Span;
}

double updateUltOsc(const DagNodeSpec &Spec, DagNodeState &State,
                    const Bar &CurrentBar) {
  const std::size_t MaxP = std::max({Spec.Period, Spec.Period2, Spec.Period3});
  if (MaxP == 0 || Spec.Period == 0 || Spec.Period2 == 0 || Spec.Period3 == 0 ||
      !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  if (!State.HasPrevClose) {
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    return kNa;
  }
  const double MinLowPrev = std::min(CurrentBar.Low, State.PrevClose);
  const double MaxHighPrev = std::max(CurrentBar.High, State.PrevClose);
  const double Bp = CurrentBar.Close - MinLowPrev;
  const double Tr = MaxHighPrev - MinLowPrev;
  if (!std::isfinite(Bp) || !std::isfinite(Tr)) {
    return kNa;
  }
  State.PrevClose = CurrentBar.Close;
  State.InputWindow.push(Bp);
  State.AuxWindow.push(Tr);
  if (State.InputWindow.size() < MaxP) {
    return kNa;
  }
  auto AvgRatio = [&](std::size_t Len) -> double {
    const double SumBp = State.InputWindow.sumLastOldestToNewest(Len);
    const double SumTr = State.AuxWindow.sumLastOldestToNewest(Len);
    if (!std::isfinite(SumBp) || !std::isfinite(SumTr)) {
      return kNa;
    }
    if (SumTr == 0.0) {
      return 0.0;
    }
    return SumBp / SumTr;
  };
  const double Avg1 = AvgRatio(Spec.Period);
  const double Avg2 = AvgRatio(Spec.Period2);
  const double Avg3 = AvgRatio(Spec.Period3);
  if (!std::isfinite(Avg1) || !std::isfinite(Avg2) || !std::isfinite(Avg3)) {
    return kNa;
  }
  return 100.0 * (4.0 * Avg1 + 2.0 * Avg2 + Avg3) / 7.0;
}

double updateElderRay(const DagNodeSpec &Spec, DagNodeState &State,
                      const Bar &CurrentBar) {
  if (Spec.Period == 0 || !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  const double EmaVal = updateEma(Spec.Period, State, CurrentBar.Close);
  if (!std::isfinite(EmaVal)) {
    return kNa;
  }
  return Spec.Kind == DagNodeKind::BullPower ? (CurrentBar.High - EmaVal)
                                             : (CurrentBar.Low - EmaVal);
}

double updateObv(DagNodeState &State, const Bar &CurrentBar) {
  if (!std::isfinite(CurrentBar.Close) || !std::isfinite(CurrentBar.Volume)) {
    return kNa;
  }
  if (!State.HasPrevClose) {
    State.PrevClose = CurrentBar.Close;
    State.HasPrevClose = true;
    State.ObvValue = 0.0;
    return State.ObvValue;
  }
  double NextObv = State.ObvValue;
  if (CurrentBar.Close > State.PrevClose) {
    NextObv += CurrentBar.Volume;
  } else if (CurrentBar.Close < State.PrevClose) {
    NextObv -= CurrentBar.Volume;
  }
  if (!std::isfinite(NextObv)) {
    return kNa;
  }
  State.PrevClose = CurrentBar.Close;
  State.ObvValue = NextObv;
  return State.ObvValue;
}

double updateMfi(std::size_t Period, DagNodeState &State,
                 const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar) ||
      !std::isfinite(CurrentBar.Volume) || CurrentBar.Volume < 0.0) {
    return kNa;
  }
  const double Tp = (CurrentBar.High + CurrentBar.Low + CurrentBar.Close) / 3.0;
  const double Rmf = Tp * CurrentBar.Volume;
  if (!std::isfinite(Tp) || !std::isfinite(Rmf)) {
    return kNa;
  }
  if (!State.HasPrevInput) {
    State.PrevInput = Tp;
    State.HasPrevInput = true;
    return kNa;
  }
  const double PosMf = (Tp > State.PrevInput) ? Rmf : 0.0;
  const double NegMf = (Tp < State.PrevInput) ? Rmf : 0.0;
  State.PrevInput = Tp;
  State.InputWindow.push(PosMf);
  State.AuxWindow.push(NegMf);
  if (State.InputWindow.size() < Period) {
    return kNa;
  }
  const double PosSum = State.InputWindow.sumOldestToNewest();
  const double NegSum = State.AuxWindow.sumOldestToNewest();
  return computeRsi(PosSum, NegSum);
}

double updatePivotFamily(const DagNodeSpec &Spec, DagNodeState &State,
                         const Bar &CurrentBar) {
  if (Spec.Period == 0 || !isValidOhlc(CurrentBar)) {
    return kNa;
  }
  double Result = kNa;
  if (State.InputWindow.size() >= Spec.Period && State.HasPrevClose) {
    const double HighestHigh = State.InputWindow.maxOldestToNewest();
    const double LowestLow = State.AuxWindow.minOldestToNewest();
    const double PrevClose = State.PrevClose;
    const double PivotVal = (HighestHigh + LowestLow + PrevClose) / 3.0;
    if (std::isfinite(HighestHigh) && std::isfinite(LowestLow) &&
        std::isfinite(PrevClose) && std::isfinite(PivotVal)) {
      switch (Spec.Kind) {
      case DagNodeKind::Pivot:
        Result = PivotVal;
        break;
      case DagNodeKind::PivotR1:
        Result = 2.0 * PivotVal - LowestLow;
        break;
      case DagNodeKind::PivotR2:
        Result = PivotVal + (HighestHigh - LowestLow);
        break;
      case DagNodeKind::PivotS1:
        Result = 2.0 * PivotVal - HighestHigh;
        break;
      case DagNodeKind::PivotS2:
        Result = PivotVal - (HighestHigh - LowestLow);
        break;
      default:
        break;
      }
    }
  }
  State.InputWindow.push(CurrentBar.High);
  State.AuxWindow.push(CurrentBar.Low);
  State.PrevClose = CurrentBar.Close;
  State.HasPrevClose = true;
  return Result;
}

double updateSuperTrend(std::size_t Period, DagNodeState &State, double Mult,
                        const Bar &CurrentBar) {
  if (Period == 0 || !isValidOhlc(CurrentBar) || !std::isfinite(Mult)) {
    return kNa;
  }
  const double PrevCloseVal = State.PrevClose;
  const bool HadPrevClose = State.HasPrevClose;
  const double AtrVal = updateAtr(Period, State, CurrentBar);
  if (!std::isfinite(AtrVal)) {
    return kNa;
  }
  const double Hl2 = (CurrentBar.High + CurrentBar.Low) / 2.0;
  const double BasicUpper = Hl2 + Mult * AtrVal;
  const double BasicLower = Hl2 - Mult * AtrVal;
  if (!std::isfinite(Hl2) || !std::isfinite(BasicUpper) ||
      !std::isfinite(BasicLower)) {
    return kNa;
  }
  if (!State.SuperTrendInitialized) {
    State.SuperTrendUpper = BasicUpper;
    State.SuperTrendLower = BasicLower;
    State.SuperTrendUp = (CurrentBar.Close >= Hl2);
    State.SuperTrendInitialized = true;
    return State.SuperTrendUp ? State.SuperTrendLower : State.SuperTrendUpper;
  }
  const double FinalUpper =
      (BasicUpper < State.SuperTrendUpper ||
       (HadPrevClose && PrevCloseVal > State.SuperTrendUpper))
          ? BasicUpper
          : State.SuperTrendUpper;
  const double FinalLower =
      (BasicLower > State.SuperTrendLower ||
       (HadPrevClose && PrevCloseVal < State.SuperTrendLower))
          ? BasicLower
          : State.SuperTrendLower;
  bool NextUp = State.SuperTrendUp;
  if (State.SuperTrendUp) {
    if (CurrentBar.Close < FinalLower) {
      NextUp = false;
    }
  } else {
    if (CurrentBar.Close > FinalUpper) {
      NextUp = true;
    }
  }
  State.SuperTrendUpper = FinalUpper;
  State.SuperTrendLower = FinalLower;
  State.SuperTrendUp = NextUp;
  return NextUp ? FinalLower : FinalUpper;
}

double updateKeltner(const DagNodeSpec &Spec, DagNodeState &State, double Mult,
                     const Bar &CurrentBar) {
  if (Spec.Period == 0 || !isValidOhlc(CurrentBar) || !std::isfinite(Mult)) {
    return kNa;
  }
  const double EmaVal =
      stepEmaTracker(State.EmaFast, Spec.Period, CurrentBar.Close);
  const double AtrVal = updateAtr(Spec.Period, State, CurrentBar);
  if (!std::isfinite(EmaVal) || !std::isfinite(AtrVal)) {
    return kNa;
  }
  return Spec.Kind == DagNodeKind::KeltnerUpper ? (EmaVal + Mult * AtrVal)
                                                : (EmaVal - Mult * AtrVal);
}

double updateLinregFamily(const DagNodeSpec &Spec, DagNodeState &State,
                          double InputA) {
  if (Spec.Period == 0 || !std::isfinite(InputA)) {
    return kNa;
  }
  State.InputWindow.push(InputA);
  if (State.InputWindow.size() < Spec.Period) {
    return kNa;
  }
  if (Spec.Period == 1) {
    return Spec.Kind == DagNodeKind::Linreg ? InputA : 0.0;
  }
  const auto PeriodD = static_cast<double>(Spec.Period);
  const double MeanY = State.InputWindow.sumOldestToNewest() / PeriodD;
  if (!std::isfinite(MeanY)) {
    return kNa;
  }
  const double MeanX = (PeriodD - 1.0) / 2.0;
  double Num = 0.0;
  for (std::size_t Idx = 0; Idx < Spec.Period; ++Idx) {
    const double YVal = State.InputWindow.at(Spec.Period - 1 - Idx);
    const double Dx = static_cast<double>(Idx) - MeanX;
    Num += Dx * (YVal - MeanY);
  }
  const double Den = PeriodD * (PeriodD * PeriodD - 1.0) / 12.0;
  if (!std::isfinite(Num) || !std::isfinite(Den) || Den == 0.0) {
    return kNa;
  }
  const double SlopeVal = Num / Den;
  if (!std::isfinite(SlopeVal)) {
    return kNa;
  }
  if (Spec.Kind == DagNodeKind::Slope) {
    return SlopeVal;
  }
  return MeanY + SlopeVal * MeanX;
}

double updateSingleBarPattern(DagNodeKind Kind, const Bar &CurrentBar) {
  if (!std::isfinite(CurrentBar.Open) || !isValidOhlc(CurrentBar) ||
      !(CurrentBar.High > CurrentBar.Low)) {
    return 0.0;
  }
  const double Range = CurrentBar.High - CurrentBar.Low;
  const double Body = std::abs(CurrentBar.Close - CurrentBar.Open);
  const double UpperWick =
      CurrentBar.High - std::max(CurrentBar.Open, CurrentBar.Close);
  const double LowerWick =
      std::min(CurrentBar.Open, CurrentBar.Close) - CurrentBar.Low;
  if (!std::isfinite(Range) || Range <= 0.0 || !std::isfinite(Body) ||
      !std::isfinite(UpperWick) || !std::isfinite(LowerWick)) {
    return 0.0;
  }
  bool Matched = false;
  switch (Kind) {
  case DagNodeKind::IsDoji:
    Matched = (Body <= 0.1 * Range);
    break;
  case DagNodeKind::IsHammer:
    Matched = (LowerWick >= 2.0 * Body && UpperWick <= 0.5 * Body);
    break;
  case DagNodeKind::IsShootingStar:
    Matched = (UpperWick >= 2.0 * Body && LowerWick <= 0.5 * Body);
    break;
  case DagNodeKind::IsPinBar: {
    const double DominantWick = std::max(UpperWick, LowerWick);
    Matched = (DominantWick >= 2.0 * Body && DominantWick >= 0.6 * Range);
    break;
  }
  default:
    break;
  }
  return Matched ? 1.0 : 0.0;
}

double updateEngulfing(DagNodeState &State, const Bar &CurrentBar,
                       bool Bullish) {
  const double CurOpen = sanitizeFinite(CurrentBar.Open);
  const double CurClose = sanitizeFinite(CurrentBar.Close);
  bool Matched = false;
  if (State.HasPrevClose && std::isfinite(State.PrevOpen) &&
      std::isfinite(State.PrevClose) && std::isfinite(CurOpen) &&
      std::isfinite(CurClose)) {
    if (Bullish) {
      Matched = (State.PrevClose < State.PrevOpen && CurClose > CurOpen &&
                 CurOpen <= State.PrevClose && CurClose >= State.PrevOpen);
    } else {
      Matched = (State.PrevClose > State.PrevOpen && CurClose < CurOpen &&
                 CurOpen >= State.PrevClose && CurClose <= State.PrevOpen);
    }
  }
  State.PrevOpen = CurOpen;
  State.PrevClose = CurClose;
  State.HasPrevClose = true;
  return Matched ? 1.0 : 0.0;
}

double updateInsideBar(DagNodeState &State, const Bar &CurrentBar) {
  const double CurHigh = sanitizeFinite(CurrentBar.High);
  const double CurLow = sanitizeFinite(CurrentBar.Low);
  bool Matched = false;
  if (State.HasPrevClose && std::isfinite(State.PrevHigh) &&
      std::isfinite(State.PrevLow) && std::isfinite(CurHigh) &&
      std::isfinite(CurLow)) {
    Matched = (CurHigh <= State.PrevHigh && CurLow >= State.PrevLow);
  }
  State.PrevHigh = CurHigh;
  State.PrevLow = CurLow;
  State.HasPrevClose = true;
  return Matched ? 1.0 : 0.0;
}

double updateSwing(const DagNodeSpec &Spec, DagNodeState &State,
                   const Bar &CurrentBar) {
  const bool IsHigh = (Spec.Kind == DagNodeKind::SwingHigh);
  const double Val = IsHigh ? CurrentBar.High : CurrentBar.Low;
  if (Spec.Period == 0 || Spec.Period2 == 0 || !std::isfinite(Val)) {
    return kNa;
  }
  const std::size_t Left = Spec.Period;
  const std::size_t Right = Spec.Period2;
  const std::size_t WinSize = Left + Right + 1;
  State.InputWindow.push(Val);
  if (State.InputWindow.size() >= WinSize) {
    const double Candidate = State.InputWindow.at(Right);
    bool Confirmed = std::isfinite(Candidate);
    for (std::size_t Off = Right + 1; Confirmed && Off <= Right + Left; ++Off) {
      const double LeftVal = State.InputWindow.at(Off);
      if (IsHigh ? !(Candidate >= LeftVal) : !(Candidate <= LeftVal)) {
        Confirmed = false;
      }
    }
    for (std::size_t Off = 0; Confirmed && Off < Right; ++Off) {
      const double RightVal = State.InputWindow.at(Off);
      if (IsHigh ? !(Candidate > RightVal) : !(Candidate < RightVal)) {
        Confirmed = false;
      }
    }
    if (Confirmed) {
      State.LastSwing = Candidate;
      State.HasSwing = true;
    }
  }
  return State.HasSwing ? State.LastSwing : kNa;
}

double updateMonotonic(std::size_t Period, DagNodeState &State, double InputA,
                       bool IsRising) {
  if (Period == 0) {
    return 0.0;
  }
  const double Current = sanitizeFinite(InputA);
  State.InputWindow.push(Current);
  if (State.InputWindow.size() <= Period) {
    return 0.0;
  }
  for (std::size_t Off = 0; Off < Period; ++Off) {
    const double Newer = State.InputWindow.at(Off);
    const double Older = State.InputWindow.at(Off + 1);
    if (!std::isfinite(Newer) || !std::isfinite(Older)) {
      return 0.0;
    }
    if (IsRising ? !(Newer > Older) : !(Newer < Older)) {
      return 0.0;
    }
  }
  return 1.0;
}

double updateBarsSince(DagNodeState &State, double InputA) {
  if (std::isfinite(InputA) && InputA != 0.0) {
    State.BarsSinceCount = 0;
    State.HasBarsSince = true;
    return 0.0;
  }
  if (!State.HasBarsSince) {
    return kNa;
  }
  ++State.BarsSinceCount;
  return static_cast<double>(State.BarsSinceCount);
}

} // namespace

DagNodeState makeDagNodeState(const DagNodeSpec &Spec) {
  DagNodeState State;
  State.Output = RingBuffer(Spec.OutputCapacity);
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
  case DagNodeKind::Ema:
  case DagNodeKind::Wma:
  case DagNodeKind::Stdev:
  case DagNodeKind::Highest:
  case DagNodeKind::Lowest:
  case DagNodeKind::BbMid:
  case DagNodeKind::BbUpper:
  case DagNodeKind::BbLower:
  case DagNodeKind::Cci:
  case DagNodeKind::BullPower:
  case DagNodeKind::BearPower:
  case DagNodeKind::Linreg:
  case DagNodeKind::Slope:
    State.InputWindow = RingBuffer(Spec.Period);
    break;
  case DagNodeKind::Change:
  case DagNodeKind::Roc:
  case DagNodeKind::Rising:
  case DagNodeKind::Falling:
    State.InputWindow = RingBuffer(Spec.Period + 1);
    break;
  case DagNodeKind::Vwap:
  case DagNodeKind::StochK:
  case DagNodeKind::WillR:
  case DagNodeKind::Mfi:
  case DagNodeKind::Pivot:
  case DagNodeKind::PivotR1:
  case DagNodeKind::PivotR2:
  case DagNodeKind::PivotS1:
  case DagNodeKind::PivotS2:
    State.InputWindow = RingBuffer(Spec.Period);
    State.AuxWindow = RingBuffer(Spec.Period);
    break;
  case DagNodeKind::KeltnerUpper:
  case DagNodeKind::KeltnerLower:
    State.EmaFast.InitWindow = RingBuffer(Spec.Period);
    break;
  case DagNodeKind::SwingHigh:
  case DagNodeKind::SwingLow:
    State.InputWindow = RingBuffer(Spec.Period + Spec.Period2 + 1);
    break;
  case DagNodeKind::StochD:
    State.InputWindow = RingBuffer(Spec.Period);
    State.AuxWindow = RingBuffer(Spec.Period);
    State.AuxWindow2 = RingBuffer(Spec.Period2);
    break;
  case DagNodeKind::StochRsi:
    State.AuxWindow = RingBuffer(Spec.Period);
    break;
  case DagNodeKind::Macd:
    State.EmaFast.InitWindow = RingBuffer(Spec.Period);
    State.EmaSlow.InitWindow = RingBuffer(Spec.Period2);
    break;
  case DagNodeKind::MacdSignal:
  case DagNodeKind::MacdHist:
    State.EmaFast.InitWindow = RingBuffer(Spec.Period);
    State.EmaSlow.InitWindow = RingBuffer(Spec.Period2);
    State.EmaSignal.InitWindow = RingBuffer(Spec.Period3);
    break;
  case DagNodeKind::UltOsc: {
    const std::size_t MaxP =
        std::max({Spec.Period, Spec.Period2, Spec.Period3});
    State.InputWindow = RingBuffer(MaxP);
    State.AuxWindow = RingBuffer(MaxP);
    break;
  }
  case DagNodeKind::Rsi:
  case DagNodeKind::Atr:
  case DagNodeKind::Adx:
  case DagNodeKind::PlusDi:
  case DagNodeKind::MinusDi:
  case DagNodeKind::Obv:
  case DagNodeKind::Crossover:
  case DagNodeKind::Crossunder:
  case DagNodeKind::SuperTrend:
  case DagNodeKind::IsDoji:
  case DagNodeKind::IsHammer:
  case DagNodeKind::IsShootingStar:
  case DagNodeKind::IsBullishEngulfing:
  case DagNodeKind::IsBearishEngulfing:
  case DagNodeKind::IsInsideBar:
  case DagNodeKind::IsPinBar:
  case DagNodeKind::BarsSince:
    break;
  }
  return State;
}

double updateDagNode(const DagNodeSpec &Spec, DagNodeState &State,
                     double InputA, double InputB, const Bar &CurrentBar) {
  double Result = kNa;
  switch (Spec.Kind) {
  case DagNodeKind::Sma:
  case DagNodeKind::Wma:
  case DagNodeKind::Stdev:
  case DagNodeKind::Highest:
  case DagNodeKind::Lowest:
  case DagNodeKind::BbMid:
  case DagNodeKind::BbUpper:
  case DagNodeKind::BbLower:
    Result = updateWindowStat(Spec, State, InputA, InputB);
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
  case DagNodeKind::Roc:
    Result = updateRoc(Spec.Period, State, InputA);
    break;
  case DagNodeKind::Crossover:
    Result = updateCross(State, InputA, InputB, true);
    break;
  case DagNodeKind::Crossunder:
    Result = updateCross(State, InputA, InputB, false);
    break;
  case DagNodeKind::Vwap:
    Result = updateVwap(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::Macd:
  case DagNodeKind::MacdSignal:
  case DagNodeKind::MacdHist:
    Result = updateMacdFamily(Spec, State, InputA);
    break;
  case DagNodeKind::StochK:
  case DagNodeKind::StochD:
    Result = updateStoch(Spec, State, CurrentBar);
    break;
  case DagNodeKind::StochRsi:
    Result = updateStochRsi(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::Adx:
  case DagNodeKind::PlusDi:
  case DagNodeKind::MinusDi:
    Result = updateDirectionalFamily(Spec, State, CurrentBar);
    break;
  case DagNodeKind::Cci:
    Result = updateCci(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::WillR:
    Result = updateWillR(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::UltOsc:
    Result = updateUltOsc(Spec, State, CurrentBar);
    break;
  case DagNodeKind::BullPower:
  case DagNodeKind::BearPower:
    Result = updateElderRay(Spec, State, CurrentBar);
    break;
  case DagNodeKind::Obv:
    Result = updateObv(State, CurrentBar);
    break;
  case DagNodeKind::Mfi:
    Result = updateMfi(Spec.Period, State, CurrentBar);
    break;
  case DagNodeKind::Pivot:
  case DagNodeKind::PivotR1:
  case DagNodeKind::PivotR2:
  case DagNodeKind::PivotS1:
  case DagNodeKind::PivotS2:
    Result = updatePivotFamily(Spec, State, CurrentBar);
    break;
  case DagNodeKind::SuperTrend:
    Result = updateSuperTrend(Spec.Period, State, InputA, CurrentBar);
    break;
  case DagNodeKind::KeltnerUpper:
  case DagNodeKind::KeltnerLower:
    Result = updateKeltner(Spec, State, InputA, CurrentBar);
    break;
  case DagNodeKind::Linreg:
  case DagNodeKind::Slope:
    Result = updateLinregFamily(Spec, State, InputA);
    break;
  case DagNodeKind::IsDoji:
  case DagNodeKind::IsHammer:
  case DagNodeKind::IsShootingStar:
  case DagNodeKind::IsPinBar:
    Result = updateSingleBarPattern(Spec.Kind, CurrentBar);
    break;
  case DagNodeKind::IsBullishEngulfing:
    Result = updateEngulfing(State, CurrentBar, true);
    break;
  case DagNodeKind::IsBearishEngulfing:
    Result = updateEngulfing(State, CurrentBar, false);
    break;
  case DagNodeKind::IsInsideBar:
    Result = updateInsideBar(State, CurrentBar);
    break;
  case DagNodeKind::SwingHigh:
  case DagNodeKind::SwingLow:
    Result = updateSwing(Spec, State, CurrentBar);
    break;
  case DagNodeKind::Rising:
    Result = updateMonotonic(Spec.Period, State, InputA, true);
    break;
  case DagNodeKind::Falling:
    Result = updateMonotonic(Spec.Period, State, InputA, false);
    break;
  case DagNodeKind::BarsSince:
    Result = updateBarsSince(State, InputA);
    break;
  }
  const double SafeResult = sanitizeFinite(Result);
  State.Output.push(SafeResult);
  return SafeResult;
}

} // namespace dsl::internal
