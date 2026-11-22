#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dsl/dsl.hpp"

namespace {

const char *const kPlatformStandard = R"dsl(# Эталон платформы
param fast = 20 range 5..50 step 5
param slow = 50 range 20..200 step 10
param rsi_n = 14
param rsi_high = 70 range 60..85 step 5
param stop_pct = 5 range 2..10 step 1
param size_pct = 95

rule "вход"
  when crossover(sma(close, fast), sma(close, slow)) and position == 0
  then buy size_pct % cash
  because "SMA({fast}) пересекла SMA({slow}) снизу вверх"

rule "стоп"
  when position > 0
  then sell all stop entry_price * (1 - stop_pct / 100)
  because "защитный стоп {stop_pct}% от цены входа {entry_price}"

rule "перекупленность"
  when position > 0 and rsi(close, rsi_n) > rsi_high
  then sell all
  because "перекупленность, RSI = {rsi(close, rsi_n)}"

rule "выход"
  when position > 0 and crossunder(sma(close, fast), sma(close, slow))
  then sell all
  because "SMA({fast}) опустилась ниже SMA({slow})"
)dsl";

dsl::Bar makeBar(std::int64_t StepVal, double CloseVal, double OpenVal = 1.0,
                 double HighVal = 0.0, double LowVal = 0.0,
                 double VolVal = 100.0) {
  dsl::Bar Result;
  Result.Step = StepVal;
  Result.Open = OpenVal;
  Result.High = HighVal > 0.0 ? HighVal : CloseVal;
  Result.Low = LowVal > 0.0 ? LowVal : CloseVal;
  Result.Close = CloseVal;
  Result.Volume = VolVal;
  return Result;
}

std::vector<dsl::Bar> makeCloseBars(const std::vector<double> &Closes) {
  std::vector<dsl::Bar> Bars;
  Bars.reserve(Closes.size());
  for (std::size_t Idx = 0; Idx < Closes.size(); ++Idx) {
    Bars.push_back(makeBar(static_cast<std::int64_t>(Idx), Closes[Idx]));
  }
  return Bars;
}

std::vector<dsl::Bar>
makeCloseOpenBars(const std::vector<std::pair<double, double>> &Pairs) {
  std::vector<dsl::Bar> Bars;
  Bars.reserve(Pairs.size());
  for (std::size_t Idx = 0; Idx < Pairs.size(); ++Idx) {
    Bars.push_back(makeBar(static_cast<std::int64_t>(Idx), Pairs[Idx].first,
                           Pairs[Idx].second));
  }
  return Bars;
}

std::vector<double> probePosExpr(std::string_view Expr,
                                 const std::vector<dsl::Bar> &Bars) {
  std::string Source = "rule \"probe\"\n  when not is_na(";
  Source += Expr;
  Source += ")\n  then buy 1 lots limit (";
  Source += Expr;
  Source += ")\n  because \"v={";
  Source += Expr;
  Source += "}\"\n";
  const auto Compiled = dsl::compile(Source, {}, 1);
  if (!Compiled.Ok || Compiled.Instance == nullptr) {
    ADD_FAILURE() << "Compile failed for probe: " << Expr;
    return {};
  }
  std::vector<double> Values;
  Values.reserve(Bars.size());
  dsl::PortfolioSnapshot Snapshot;
  Snapshot.Cash = 1'000'000.0;
  Snapshot.Equity = 1'000'000.0;
  for (const auto &CurrentBar : Bars) {
    const auto Outcome =
        dsl::step(*Compiled.Instance, 0, CurrentBar, Snapshot, false);
    if (Outcome.Error.has_value()) {
      ADD_FAILURE() << "Step error: " << Outcome.Error->Message;
      return Values;
    }
    if (Outcome.Intents.empty()) {
      Values.push_back(dsl::kNa);
    } else {
      Values.push_back(Outcome.Intents.front().RawPrice);
    }
  }
  return Values;
}

TEST(FormatBecauseTest, Rf1RoundingAndSpecialValues) {
  EXPECT_EQ(dsl::formatBecauseNumber(1.005), "1,01");
  EXPECT_EQ(dsl::formatBecauseNumber(70.004), "70");
  EXPECT_EQ(dsl::formatBecauseNumber(70.10), "70,1");
  EXPECT_EQ(dsl::formatBecauseNumber(-0.004), "0");
  EXPECT_EQ(dsl::formatBecauseNumber(-0.0), "0");
  EXPECT_EQ(dsl::formatBecauseNumber(-12.345), "-12,35");
  EXPECT_EQ(dsl::formatBecauseNumber(dsl::kNa), "н/д");
  const double Inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(dsl::formatBecauseNumber(Inf), "н/д");
  EXPECT_EQ(dsl::formatBecauseNumber(-Inf), "н/д");
  EXPECT_EQ(dsl::formatBecauseNumber(1e15), "1000000000000000");
}

TEST(FormatBecauseTest, TemplateEscapingBoolAndNa) {
  const std::string Source = R"dsl(
rule "fmt"
  when true
  then buy 1 lots
  because "{{x}}={1.005}; ok={true}; fail={false}; na={sqrt(-1)}"
)dsl";
  auto Compiled = dsl::compile(Source, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_NE(Compiled.Instance, nullptr);
  dsl::PortfolioSnapshot Snapshot;
  const auto Outcome =
      dsl::step(*Compiled.Instance, 0, makeBar(0, 10.0), Snapshot, false);
  ASSERT_FALSE(Outcome.Error.has_value());
  ASSERT_EQ(Outcome.Intents.size(), 1U);
  EXPECT_EQ(Outcome.Intents[0].Because, "{x}=1,01; ok=да; fail=нет; na=н/д");
}

TEST(IndicatorsTest, SmaWorkedExampleAndNonDrift) {
  const auto Bars =
      makeCloseOpenBars({{10, 1}, {20, 1}, {99, 0}, {30, 1}, {40, 1}});
  const auto Out = probePosExpr("sma(close + 0 / open, 3)", Bars);
  ASSERT_EQ(Out.size(), 5U);
  EXPECT_TRUE(dsl::isNaValue(Out[0]));
  EXPECT_TRUE(dsl::isNaValue(Out[1]));
  EXPECT_TRUE(dsl::isNaValue(Out[2]));
  EXPECT_DOUBLE_EQ(Out[3], 20.0);
  EXPECT_DOUBLE_EQ(Out[4], 30.0);

  const std::string DriftSrc = R"dsl(
rule "drift"
  when sma(close, 2) == 1.5
  then buy 1 lots limit sma(close, 2)
  because "sma={sma(close, 2)}"
)dsl";
  auto Compiled = dsl::compile(DriftSrc, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  dsl::PortfolioSnapshot Snapshot;
  const std::vector<double> Closes = {1e16, 1.0, 2.0};
  auto &Inst = *Compiled.Instance;
  for (std::size_t Idx = 0; Idx < Closes.size(); ++Idx) {
    const auto BarNow = makeBar(static_cast<std::int64_t>(Idx), Closes[Idx]);
    const auto StepRes = dsl::step(Inst, 0, BarNow, Snapshot, false);
    ASSERT_FALSE(StepRes.Error.has_value());
    if (Idx < 2U) {
      EXPECT_TRUE(StepRes.Intents.empty());
    } else {
      ASSERT_EQ(StepRes.Intents.size(), 1U);
      EXPECT_DOUBLE_EQ(StepRes.Intents[0].RawPrice, 1.5);
    }
  }
}

TEST(IndicatorsTest, EmaPreservesStateAcrossNa) {
  const auto Bars =
      makeCloseOpenBars({{10, 1}, {12, 1}, {14, 1}, {99, 0}, {16, 1}});
  const auto Out = probePosExpr("ema(close + 0 / open, 3)", Bars);
  ASSERT_EQ(Out.size(), 5U);
  EXPECT_TRUE(dsl::isNaValue(Out[0]));
  EXPECT_TRUE(dsl::isNaValue(Out[1]));
  EXPECT_DOUBLE_EQ(Out[2], 12.0);
  EXPECT_TRUE(dsl::isNaValue(Out[3]));
  EXPECT_DOUBLE_EQ(Out[4], 14.0);
}

TEST(IndicatorsTest, RsiWorkedExamplesAndSubnormalAvgLoss) {
  const auto RsiBars = makeCloseBars({100.0, 102.0, 101.0, 104.0});
  const auto Out = probePosExpr("rsi(close, 2)", RsiBars);
  ASSERT_EQ(Out.size(), 4U);
  EXPECT_TRUE(dsl::isNaValue(Out[0]));
  EXPECT_TRUE(dsl::isNaValue(Out[1]));
  EXPECT_NEAR(Out[2], 200.0 / 3.0, 1e-12);
  EXPECT_NEAR(Out[3], 800.0 / 9.0, 1e-12);

  const auto FlatBars = makeCloseBars({100.0, 100.0, 100.0});
  const auto Flat = probePosExpr("rsi(close, 2)", FlatBars);
  ASSERT_EQ(Flat.size(), 3U);
  EXPECT_DOUBLE_EQ(Flat[2], 50.0);

  const auto UpBars = makeCloseBars({100.0, 101.0, 102.0});
  const auto Up = probePosExpr("rsi(close, 2)", UpBars);
  ASSERT_EQ(Up.size(), 3U);
  EXPECT_DOUBLE_EQ(Up[2], 100.0);

  std::vector<dsl::Bar> LongRise = {makeBar(0, 100.0), makeBar(1, 102.0),
                                    makeBar(2, 101.0)};
  for (int StepIdx = 3; StepIdx < 2500; ++StepIdx) {
    LongRise.push_back(makeBar(StepIdx, 100.0 + StepIdx));
  }
  const auto LongOut = probePosExpr("rsi(close, 2)", LongRise);
  ASSERT_EQ(LongOut.size(), LongRise.size());
  EXPECT_FALSE(dsl::isNaValue(LongOut.back()));
  EXPECT_DOUBLE_EQ(LongOut.back(), 100.0);
}

TEST(IndicatorsTest, AtrWorkedExample) {
  const std::vector<dsl::Bar> Bars = {
      makeBar(0, 102.0, 101.0, 105.0, 100.0),
      makeBar(1, 106.0, 105.0, 108.0, 104.0),
      makeBar(2, 103.0, 105.0, 107.0, 101.0),
  };
  const auto Out = probePosExpr("atr(2)", Bars);
  ASSERT_EQ(Out.size(), 3U);
  EXPECT_TRUE(dsl::isNaValue(Out[0]));
  EXPECT_DOUBLE_EQ(Out[1], 5.5);
  EXPECT_DOUBLE_EQ(Out[2], 5.75);
}

TEST(IndicatorsTest, StdevWorkedExamplesAndTwoPass) {
  const auto StdevBars = makeCloseBars({10.0, 12.0, 14.0});
  const auto Out = probePosExpr("stdev(close, 3)", StdevBars);
  ASSERT_EQ(Out.size(), 3U);
  EXPECT_TRUE(dsl::isNaValue(Out[0]));
  EXPECT_TRUE(dsl::isNaValue(Out[1]));
  EXPECT_DOUBLE_EQ(Out[2], std::sqrt(8.0 / 3.0));

  const auto OffsetBars = makeCloseBars({1e9 + 1.0, 1e9 + 2.0, 1e9 + 3.0});
  const auto Offset = probePosExpr("stdev(close, 3)", OffsetBars);
  ASSERT_EQ(Offset.size(), 3U);
  EXPECT_DOUBLE_EQ(Offset[2], std::sqrt(2.0 / 3.0));

  const std::string ZeroSrc = R"dsl(
rule "stdev1"
  when stdev(close, 1) == 0
  then buy 1 lots
  because "s={stdev(close, 1)}"
)dsl";
  auto Compiled = dsl::compile(ZeroSrc, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  dsl::PortfolioSnapshot Snapshot;
  const auto StepRes =
      dsl::step(*Compiled.Instance, 0, makeBar(0, 42.0), Snapshot, false);
  ASSERT_EQ(StepRes.Intents.size(), 1U);
  EXPECT_EQ(StepRes.Intents[0].Because, "s=0");
}

TEST(IndicatorsTest, HighestLowestChangeCrossoverAndMath) {
  const std::vector<dsl::Bar> HiLoBars = {
      makeBar(0, 5.0), makeBar(1, 2.0), makeBar(2, 8.0),
      makeBar(3, 1.0), makeBar(4, 4.0),
  };
  const auto Hi = probePosExpr("highest(close, 3)", HiLoBars);
  const auto Lo = probePosExpr("lowest(close, 3)", HiLoBars);
  ASSERT_EQ(Hi.size(), 5U);
  EXPECT_TRUE(dsl::isNaValue(Hi[0]));
  EXPECT_TRUE(dsl::isNaValue(Hi[1]));
  EXPECT_DOUBLE_EQ(Hi[2], 8.0);
  EXPECT_DOUBLE_EQ(Hi[3], 8.0);
  EXPECT_DOUBLE_EQ(Hi[4], 8.0);
  EXPECT_DOUBLE_EQ(Lo[2], 2.0);
  EXPECT_DOUBLE_EQ(Lo[3], 1.0);
  EXPECT_DOUBLE_EQ(Lo[4], 1.0);

  const auto ChgBars =
      makeCloseOpenBars({{100, 1}, {99, 0}, {105, 1}, {110, 1}});
  const auto Chg = probePosExpr("change(close + 0 / open, 2)", ChgBars);
  ASSERT_EQ(Chg.size(), 4U);
  EXPECT_TRUE(dsl::isNaValue(Chg[0]));
  EXPECT_TRUE(dsl::isNaValue(Chg[1]));
  EXPECT_DOUBLE_EQ(Chg[2], 5.0);
  EXPECT_TRUE(dsl::isNaValue(Chg[3]));

  const std::string CrossSrc = R"dsl(
rule "up"
  when crossover(close + 0 / open, 10)
  then buy 1 lots
  because "up"
rule "dn"
  when crossunder(close + 0 / open, 10)
  then sell 1 lots
  because "dn"
rule "math"
  when is_na(sqrt(-1)) and is_na(min(close, sqrt(-1)))
       and abs(-5) == 5 and max(3, 7) == 7 and min(3, 7) == 3
  then buy 1 lots limit sqrt(9)
  because "m"
)dsl";
  auto Compiled = dsl::compile(CrossSrc, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  dsl::PortfolioSnapshot Snapshot;
  const auto CrossBars =
      makeCloseOpenBars({{12, 1}, {10, 1}, {12, 1}, {99, 0}, {12, 1}, {8, 1}});
  std::vector<std::string> Fired;
  auto &Inst = *Compiled.Instance;
  for (const auto &BarItem : CrossBars) {
    const auto Res = dsl::step(Inst, 0, BarItem, Snapshot, false);
    ASSERT_FALSE(Res.Error.has_value());
    for (const auto &Intent : Res.Intents) {
      if (Intent.RuleName == "math") {
        EXPECT_DOUBLE_EQ(Intent.RawPrice, 3.0);
      } else {
        Fired.push_back(std::to_string(BarItem.Step) + ":" + Intent.RuleName);
      }
    }
  }
  const std::vector<std::string> Expected = {"2:up", "5:dn"};
  EXPECT_EQ(Fired, Expected);
}

TEST(EngineTest, WarmupAndErT01NonIncreasingStep) {
  const std::string Src = R"dsl(
rule "w"
  when sma(close, 3) > 10
  then buy 1 lots
  because "sma={sma(close, 3)}"
)dsl";
  auto Compiled = dsl::compile(Src, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  dsl::PortfolioSnapshot Snapshot;
  const auto Warm0 =
      dsl::step(*Compiled.Instance, 0, makeBar(0, 11.0), Snapshot, true);
  EXPECT_TRUE(Warm0.Intents.empty());
  const auto Warm1 =
      dsl::step(*Compiled.Instance, 0, makeBar(1, 12.0), Snapshot, true);
  EXPECT_TRUE(Warm1.Intents.empty());
  const auto Active =
      dsl::step(*Compiled.Instance, 0, makeBar(2, 13.0), Snapshot, false);
  ASSERT_FALSE(Active.Error.has_value());
  ASSERT_EQ(Active.Intents.size(), 1U);
  EXPECT_EQ(Active.Intents[0].Because, "sma=12");

  const auto SameStep =
      dsl::step(*Compiled.Instance, 0, makeBar(2, 14.0), Snapshot, false);
  ASSERT_TRUE(SameStep.Error.has_value());
  EXPECT_EQ(SameStep.Error->Code, "E-RT01");

  const auto BackStep =
      dsl::step(*Compiled.Instance, 0, makeBar(1, 14.0), Snapshot, false);
  ASSERT_TRUE(BackStep.Error.has_value());
  EXPECT_EQ(BackStep.Error->Code, "E-RT01");
}

TEST(EngineTest, MultiInstrumentIsolation) {
  const std::string Src = R"dsl(
rule "inst"
  when sma(close, 2) > 0
  then buy 1 lots limit sma(close, 2)
  because "s={sma(close, 2)}"
)dsl";
  auto Compiled = dsl::compile(Src, {}, 2);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_EQ(Compiled.Instance->instrumentCount(), 2U);
  dsl::PortfolioSnapshot Snapshot;
  ASSERT_FALSE(
      dsl::step(*Compiled.Instance, 0, makeBar(0, 10.0), Snapshot, false)
          .Error.has_value());
  ASSERT_FALSE(
      dsl::step(*Compiled.Instance, 1, makeBar(10, 100.0), Snapshot, false)
          .Error.has_value());
  const auto Out0 =
      dsl::step(*Compiled.Instance, 0, makeBar(1, 20.0), Snapshot, false);
  const auto Out1 =
      dsl::step(*Compiled.Instance, 1, makeBar(11, 200.0), Snapshot, false);
  ASSERT_EQ(Out0.Intents.size(), 1U);
  ASSERT_EQ(Out1.Intents.size(), 1U);
  EXPECT_DOUBLE_EQ(Out0.Intents[0].RawPrice, 15.0);
  EXPECT_DOUBLE_EQ(Out1.Intents[0].RawPrice, 150.0);
  EXPECT_TRUE(
      dsl::step(*Compiled.Instance, 0, makeBar(1, 30.0), Snapshot, false)
          .Error.has_value());
  EXPECT_FALSE(
      dsl::step(*Compiled.Instance, 1, makeBar(12, 300.0), Snapshot, false)
          .Error.has_value());
}

TEST(EngineTest, ActionSizingBoundsShortCircuitAndLazyBecause) {
  const std::string Src = R"dsl(
rule "cash_buy"
  when close == 250
  then buy 95 % cash
  because "c={cash}"
rule "huge_lots"
  when close == 10
  then buy 10000000000 * 10000000000 lots
  because "drop"
rule "huge_limit"
  when close == 10
  then buy 1 lots limit 10000000000 * 10000000000
  because "drop"
rule "neg_limit"
  when close == 10
  then buy 1 lots limit -5
  because "drop"
rule "zero_limit"
  when close == 10
  then buy 1 lots limit 0
  because "drop"
rule "sub_one_lot"
  when close == 10
  then buy 0.5 lots
  because "{1 / 0}"
rule "false_lazy"
  when false
  then buy 1 lots
  because "{1 / 0}"
rule "eq_buy"
  when close == 250
  then buy 50 % equity
  because "eq"
rule "pos_buy"
  when position >= 4 and close == 10
  then buy 100 % position
  because "pb"
rule "half_sell"
  when position >= 4 and (false and (close / 0 > 0)) == false
  then sell 50 % position day
  because "pos={position}"
rule "cancel_all"
  when true or (close / 0 > 0)
  then cancel
  because "cancel at {close}"
)dsl";
  auto Compiled = dsl::compile(Src, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  dsl::PortfolioSnapshot Snap;
  Snap.Cash = 1'000'000.0;
  Snap.Equity = 1'000'000.0;
  Snap.LotSize = 10;
  const auto Res0 =
      dsl::step(*Compiled.Instance, 0, makeBar(0, 250.0), Snap, false);
  ASSERT_EQ(Res0.Intents.size(), 3U);
  EXPECT_EQ(Res0.Intents[0].Kind, dsl::ActionKind::Buy);
  EXPECT_EQ(Res0.Intents[0].Lots, 380);
  EXPECT_EQ(Res0.Intents[1].Kind, dsl::ActionKind::Buy);
  EXPECT_EQ(Res0.Intents[1].Lots, 200);
  EXPECT_EQ(Res0.Intents[2].Kind, dsl::ActionKind::Cancel);
  EXPECT_EQ(Res0.Intents[2].Because, "cancel at 250");

  Snap.Position = 5;
  const auto Res1 =
      dsl::step(*Compiled.Instance, 0, makeBar(1, 10.0), Snap, false);
  ASSERT_EQ(Res1.Intents.size(), 3U);
  EXPECT_EQ(Res1.Intents[0].Kind, dsl::ActionKind::Buy);
  EXPECT_EQ(Res1.Intents[0].Lots, 5);
  EXPECT_EQ(Res1.Intents[1].Kind, dsl::ActionKind::Sell);
  EXPECT_EQ(Res1.Intents[1].Lots, 2);
  EXPECT_EQ(Res1.Intents[1].Tif, dsl::TimeInForce::Day);
  EXPECT_EQ(Res1.Intents[2].Kind, dsl::ActionKind::Cancel);
}

TEST(EngineTest, Ft19DivergenceModeAndPlatformStandard) {
  const std::string DivSrc = R"dsl(
rule "flat"
  when position == 0 and is_na(entry_price) and is_na(bars_in_position)
  then buy 1 lots
  because "flat"
rule "held"
  when position > 0 and bars_in_position >= 3 and close < entry_price
  then sell all
  because "held={bars_in_position}; ep={entry_price}"
)dsl";
  auto DivCompiled = dsl::compile(DivSrc, {}, 1);
  ASSERT_TRUE(DivCompiled.Ok);
  dsl::PortfolioSnapshot Human;
  Human.Position = 0;
  Human.EntryPrice = dsl::kNa;
  Human.BarsInPosition = std::nullopt;
  const auto FlatRes =
      dsl::step(*DivCompiled.Instance, 0, makeBar(0, 100.0), Human, false);
  ASSERT_EQ(FlatRes.Intents.size(), 1U);
  EXPECT_EQ(FlatRes.Intents[0].RuleName, "flat");

  Human.Position = 4;
  Human.EntryPrice = 110.0;
  Human.BarsInPosition = 3;
  const auto HeldRes =
      dsl::step(*DivCompiled.Instance, 0, makeBar(1, 105.0), Human, false);
  ASSERT_EQ(HeldRes.Intents.size(), 1U);
  EXPECT_EQ(HeldRes.Intents[0].RuleName, "held");
  EXPECT_EQ(HeldRes.Intents[0].Lots, 4);
  EXPECT_EQ(HeldRes.Intents[0].Because, "held=3; ep=110");

  auto StdA = dsl::compile(kPlatformStandard, {{"size_pct", 31.0}}, 1);
  auto StdB = dsl::compile(kPlatformStandard, {{"size_pct", 31.0}}, 1);
  ASSERT_TRUE(StdA.Ok);
  ASSERT_TRUE(StdB.Ok);
  EXPECT_LE(StdA.Instance->dagNodeCount(), dsl::kMaxDagNodes);

  dsl::PortfolioSnapshot Port;
  Port.Cash = 100'000.0;
  Port.Equity = 100'000.0;
  for (int WarmIdx = 0; WarmIdx < 200; ++WarmIdx) {
    ASSERT_TRUE(
        dsl::step(*StdA.Instance, 0, makeBar(WarmIdx, 100.0), Port, true)
            .Intents.empty());
    ASSERT_TRUE(
        dsl::step(*StdB.Instance, 0, makeBar(WarmIdx, 100.0), Port, true)
            .Intents.empty());
  }
  const auto EntryA =
      dsl::step(*StdA.Instance, 0, makeBar(200, 120.0), Port, false);
  const auto EntryB =
      dsl::step(*StdB.Instance, 0, makeBar(200, 120.0), Port, false);
  EXPECT_EQ(EntryA.Intents, EntryB.Intents);
  ASSERT_EQ(EntryA.Intents.size(), 1U);
  EXPECT_EQ(EntryA.Intents[0].RuleName, "вход");
  EXPECT_EQ(EntryA.Intents[0].Lots, 258);
  const std::string ExpBecause = "SMA(20) пересекла SMA(50) снизу вверх";
  EXPECT_EQ(EntryA.Intents[0].Because, ExpBecause);

  Port.Position = 258;
  Port.EntryPrice = 120.0;
  Port.BarsInPosition = 0;
  const auto ActiveA =
      dsl::step(*StdA.Instance, 0, makeBar(201, 121.0), Port, false);
  const auto ActiveB =
      dsl::step(*StdB.Instance, 0, makeBar(201, 121.0), Port, false);
  EXPECT_EQ(ActiveA.Intents, ActiveB.Intents);
  ASSERT_EQ(ActiveA.Intents.size(), 2U);
  EXPECT_EQ(ActiveA.Intents[0].RuleName, "стоп");
  EXPECT_EQ(ActiveA.Intents[0].OrderKind, dsl::OrderType::Stop);
  EXPECT_DOUBLE_EQ(ActiveA.Intents[0].RawPrice, 114.0);
  EXPECT_EQ(ActiveA.Intents[1].RuleName, "перекупленность");

  bool SawExit = false;
  for (int StepIdx = 202; StepIdx < 225; ++StepIdx) {
    const auto OutA =
        dsl::step(*StdA.Instance, 0, makeBar(StepIdx, 50.0), Port, false);
    const auto OutB =
        dsl::step(*StdB.Instance, 0, makeBar(StepIdx, 50.0), Port, false);
    EXPECT_EQ(OutA.Intents, OutB.Intents);
    for (const auto &Intent : OutA.Intents) {
      if (Intent.RuleName == "выход") {
        SawExit = true;
      }
    }
  }
  EXPECT_TRUE(SawExit);
  EXPECT_FALSE(StdA.Instance->isCancelled());
  StdA.Instance->requestCancel();
  EXPECT_TRUE(StdA.Instance->isCancelled());
  const auto AfterCancel =
      dsl::step(*StdA.Instance, 0, makeBar(225, 50.0), Port, false);
  EXPECT_FALSE(AfterCancel.Error.has_value());
  EXPECT_TRUE(AfterCancel.Intents.empty());
}

TEST(EngineTest, RecursiveIndicatorsDoNotPoisonStateOnOverflow) {
  const std::string Src = R"dsl(
rule "ema_p" when not is_na(ema(close, 2))
  then cancel because "{ema(close, 2)}"
rule "rsi_p" when not is_na(rsi(close, 2))
  then cancel because "{rsi(close, 2)}"
rule "atr_p" when not is_na(atr(2))
  then cancel because "{atr(2)}"
)dsl";
  auto Compiled = dsl::compile(Src, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_NE(Compiled.Instance, nullptr);

  dsl::PortfolioSnapshot Snap;
  Snap.Cash = 1'000'000.0;
  Snap.Equity = 1'000'000.0;

  auto RunBar = [&](std::int64_t StepIdx, double High, double Low,
                    double Close) {
    dsl::Bar BarVal;
    BarVal.Step = StepIdx;
    BarVal.Open = Close;
    BarVal.High = High;
    BarVal.Low = Low;
    BarVal.Close = Close;
    BarVal.Volume = 100.0;
    return dsl::step(*Compiled.Instance, 0, BarVal, Snap, false);
  };
  auto HasRule = [](const dsl::StepOutcome &StepRes, std::string_view Name) {
    for (const auto &Intent : StepRes.Intents) {
      if (Intent.RuleName == Name) {
        return true;
      }
    }
    return false;
  };

  // Warmup overflow for ema(2), rsi(2), atr(2) must not poison later bars.
  const auto S0 = RunBar(0, 1e308, -1e308, 1e308);
  EXPECT_FALSE(HasRule(S0, "ema_p"));
  EXPECT_FALSE(HasRule(S0, "rsi_p"));
  EXPECT_FALSE(HasRule(S0, "atr_p"));

  const auto S1 = RunBar(1, 1e308, -1e308, 1e308);
  EXPECT_FALSE(HasRule(S1, "ema_p"));
  EXPECT_FALSE(HasRule(S1, "rsi_p"));
  EXPECT_FALSE(HasRule(S1, "atr_p"));

  // Feed normal finite bars; recursive indicators must recover cleanly.
  const auto S2 = RunBar(2, 102.0, 98.0, 100.0);
  const auto S3 = RunBar(3, 104.0, 100.0, 102.0);
  const auto S4 = RunBar(4, 106.0, 102.0, 104.0);
  EXPECT_FALSE(S2.Error.has_value());
  EXPECT_FALSE(S3.Error.has_value());
  EXPECT_FALSE(S4.Error.has_value());
  EXPECT_TRUE(HasRule(S4, "ema_p"));
  EXPECT_TRUE(HasRule(S4, "rsi_p"));
  EXPECT_TRUE(HasRule(S4, "atr_p"));

  // Post-warmup overflow spike (-1e308 -> +1e308 gives +Inf Delta and Tr)
  // must return kNa on that bar and preserve state.
  const auto SpikeBase = RunBar(5, 1e308, 0.0, -1e308);
  EXPECT_FALSE(SpikeBase.Error.has_value());
  const auto Spike = RunBar(6, 1e308, -1e308, 1e308);
  EXPECT_FALSE(HasRule(Spike, "rsi_p"));
  EXPECT_FALSE(HasRule(Spike, "atr_p"));

  const auto AfterSpike = RunBar(7, 108.0, 104.0, 106.0);
  EXPECT_FALSE(AfterSpike.Error.has_value());
  EXPECT_TRUE(HasRule(AfterSpike, "rsi_p"));
  EXPECT_TRUE(HasRule(AfterSpike, "atr_p"));
}

} // namespace
