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

std::vector<double> probeSignedExpr(std::string_view Expr,
                                    const std::vector<dsl::Bar> &Bars,
                                    double Shift = 10000.0) {
  std::string Shifted = "(";
  Shifted += Expr;
  Shifted += ") + ";
  Shifted += std::to_string(Shift);
  auto Raw = probePosExpr(Shifted, Bars);
  for (double &Val : Raw) {
    if (!dsl::isNaValue(Val)) {
      Val -= Shift;
    }
  }
  return Raw;
}

TEST(IndicatorsTest, TaMovingAveragesAndBands) {
  // WMA(close, 3) on {10, 20, 30, 40}:
  // bar 2: (1*10 + 2*20 + 3*30) / 6 = 140 / 6
  // bar 3: (1*20 + 2*30 + 3*40) / 6 = 200 / 6
  const auto Bars = makeCloseBars({10.0, 20.0, 30.0, 40.0});
  const auto WmaOut = probePosExpr("wma(close, 3)", Bars);
  ASSERT_EQ(WmaOut.size(), 4U);
  EXPECT_TRUE(dsl::isNaValue(WmaOut[0]));
  EXPECT_TRUE(dsl::isNaValue(WmaOut[1]));
  EXPECT_NEAR(WmaOut[2], 140.0 / 6.0, 1e-12);
  EXPECT_NEAR(WmaOut[3], 200.0 / 6.0, 1e-12);

  // Bollinger Bands on {10, 12, 14}: mean = 12, stdev = sqrt(8/3)
  const auto BbBars = makeCloseBars({10.0, 12.0, 14.0});
  const auto BbMid = probePosExpr("bb_mid(close, 3)", BbBars);
  const auto BbUp = probePosExpr("bb_upper(close, 3, 2)", BbBars);
  const auto BbLo = probePosExpr("bb_lower(close, 3, 2)", BbBars);
  const double Sd = std::sqrt(8.0 / 3.0);
  EXPECT_TRUE(dsl::isNaValue(BbMid[1]));
  EXPECT_DOUBLE_EQ(BbMid[2], 12.0);
  EXPECT_NEAR(BbUp[2], 12.0 + 2.0 * Sd, 1e-12);
  EXPECT_NEAR(BbLo[2], 12.0 - 2.0 * Sd, 1e-12);

  // Rolling VWAP(2):
  // bar 0: tp = (12+9+12)/3 = 11, vol = 100
  // bar 1: tp = (15+12+15)/3 = 14, vol = 300 -> (1100 + 4200)/400 = 13.25
  // bar 2: tp = 20, vol = 0 -> (4200 + 0)/300 = 14.0
  // bar 3: tp = 20, vol = 0 -> sumVol == 0 -> na
  const std::vector<dsl::Bar> VwapBars = {
      makeBar(0, 12.0, 10.0, 12.0, 9.0, 100.0),
      makeBar(1, 15.0, 13.0, 15.0, 12.0, 300.0),
      makeBar(2, 20.0, 20.0, 20.0, 20.0, 0.0),
      makeBar(3, 20.0, 20.0, 20.0, 20.0, 0.0),
  };
  const auto VwapOut = probePosExpr("vwap(2)", VwapBars);
  ASSERT_EQ(VwapOut.size(), 4U);
  EXPECT_TRUE(dsl::isNaValue(VwapOut[0]));
  EXPECT_DOUBLE_EQ(VwapOut[1], 13.25);
  EXPECT_DOUBLE_EQ(VwapOut[2], 14.0);
  EXPECT_TRUE(dsl::isNaValue(VwapOut[3]));
}

TEST(IndicatorsTest, TaMacdFamilyAndRoc) {
  // EMA(2) alpha=2/3, EMA(3) alpha=1/2 on {10, 12, 14, 16, 18}:
  // bar 0: fast=na, slow=na
  // bar 1: fast=11, slow=na
  // bar 2: fast=2/3*14 + 1/3*11 = 13, slow=(10+12+14)/3 = 12 -> macd=1
  // bar 3: fast=2/3*16 + 1/3*13 = 15, slow=0.5*16 + 0.5*12 = 14 -> macd=1
  // bar 4: fast=2/3*18 + 1/3*15 = 17, slow=0.5*18 + 0.5*14 = 16 -> macd=1
  // signal(2) at bar 3 = (1+1)/2 = 1, hist = 0
  const auto Bars = makeCloseBars({10.0, 12.0, 14.0, 16.0, 22.0});
  const auto MacdOut = probeSignedExpr("macd(close, 2, 3)", Bars);
  const auto SigOut = probeSignedExpr("macd_signal(close, 2, 3, 2)", Bars);
  const auto HistOut = probeSignedExpr("macd_hist(close, 2, 3, 2)", Bars);
  ASSERT_EQ(MacdOut.size(), 5U);
  EXPECT_TRUE(dsl::isNaValue(MacdOut[0]));
  EXPECT_TRUE(dsl::isNaValue(MacdOut[1]));
  EXPECT_NEAR(MacdOut[2], 1.0, 1e-9);
  EXPECT_NEAR(MacdOut[3], 1.0, 1e-9);
  // bar 4: fast = 2/3*22 + 1/3*15 = 59/3, slow = 0.5*22 + 0.5*14 = 18
  // macd = 59/3 - 18 = 5/3; sig = 2/3*(5/3) + 1/3*1 = 13/9; hist = 2/9
  EXPECT_NEAR(MacdOut[4], 5.0 / 3.0, 1e-9);
  EXPECT_TRUE(dsl::isNaValue(SigOut[2]));
  EXPECT_NEAR(SigOut[3], 1.0, 1e-9);
  EXPECT_NEAR(SigOut[4], 13.0 / 9.0, 1e-9);
  EXPECT_TRUE(dsl::isNaValue(HistOut[2]));
  EXPECT_NEAR(HistOut[3], 0.0, 1e-9);
  EXPECT_NEAR(HistOut[4], 2.0 / 9.0, 1e-9);

  // ROC(close, 2) on {100, 0, 120, 150}:
  // bar 0,1: na; bar 2: (120-100)/100*100 = 20; bar 3: past=0 -> na
  const auto RocBars = makeCloseBars({100.0, 0.0, 120.0, 150.0});
  const auto RocOut = probeSignedExpr("roc(close, 2)", RocBars);
  ASSERT_EQ(RocOut.size(), 4U);
  EXPECT_TRUE(dsl::isNaValue(RocOut[0]));
  EXPECT_TRUE(dsl::isNaValue(RocOut[1]));
  EXPECT_NEAR(RocOut[2], 20.0, 1e-9);
  EXPECT_TRUE(dsl::isNaValue(RocOut[3]));
}

TEST(IndicatorsTest, TaStochasticsWilliamsAndCci) {
  const std::vector<dsl::Bar> Bars = {
      makeBar(0, 12.0, 10.0, 14.0, 10.0),
      makeBar(1, 15.0, 12.0, 16.0, 11.0),
      makeBar(2, 14.0, 15.0, 18.0, 12.0),
      makeBar(3, 18.0, 14.0, 18.0, 14.0),
  };
  // stoch_k(2):
  // bar 1: hi=16, lo=10, close=15 -> 100*(15-10)/6 = 83.3333...
  // bar 2: hi=18, lo=11, close=14 -> 100*(14-11)/7 = 300/7
  // bar 3: hi=18, lo=12, close=18 -> 100*(18-12)/6 = 100
  const auto KOut = probeSignedExpr("stoch_k(2)", Bars);
  const auto DOut = probeSignedExpr("stoch_d(2, 2)", Bars);
  const auto WillOut = probeSignedExpr("willr(2)", Bars);
  EXPECT_TRUE(dsl::isNaValue(KOut[0]));
  EXPECT_NEAR(KOut[1], 500.0 / 6.0, 1e-9);
  EXPECT_NEAR(KOut[2], 300.0 / 7.0, 1e-9);
  EXPECT_NEAR(KOut[3], 100.0, 1e-9);

  EXPECT_TRUE(dsl::isNaValue(DOut[1]));
  EXPECT_NEAR(DOut[2], (500.0 / 6.0 + 300.0 / 7.0) / 2.0, 1e-9);
  EXPECT_NEAR(DOut[3], (300.0 / 7.0 + 100.0) / 2.0, 1e-9);

  EXPECT_TRUE(dsl::isNaValue(WillOut[0]));
  EXPECT_NEAR(WillOut[1], -100.0 * (16.0 - 15.0) / 6.0, 1e-9);
  EXPECT_NEAR(WillOut[3], 0.0, 1e-9);

  // Flat bars -> stoch_k = 50, willr = -50, cci = 0
  const auto FlatBars = makeCloseBars({100.0, 100.0, 100.0});
  const auto FlatK = probeSignedExpr("stoch_k(2)", FlatBars);
  const auto FlatW = probeSignedExpr("willr(2)", FlatBars);
  const auto FlatCci = probeSignedExpr("cci(2)", FlatBars);
  EXPECT_NEAR(FlatK[2], 50.0, 1e-9);
  EXPECT_NEAR(FlatW[2], -50.0, 1e-9);
  EXPECT_NEAR(FlatCci[2], 0.0, 1e-9);

  // CCI(3) on tp = {10, 12, 14}: mean=12, mad=(2+0+2)/3=4/3
  // cci = (14-12)/(0.015 * 4/3) = 2 / 0.02 = 100
  const auto CciBars = makeCloseBars({10.0, 12.0, 14.0});
  const auto CciOut = probeSignedExpr("cci(3)", CciBars);
  EXPECT_TRUE(dsl::isNaValue(CciOut[1]));
  EXPECT_NEAR(CciOut[2], 100.0, 1e-9);

  // StochRSI(2): RSI(2) needs 3 bars (bar 2), then window of 2 RSI values
  // completes at bar 3 (2 * 2 = 4 bars).
  const auto SrsiBars = makeCloseBars({100.0, 102.0, 101.0, 104.0, 100.0});
  const auto SrsiOut = probeSignedExpr("stoch_rsi(2)", SrsiBars);
  EXPECT_TRUE(dsl::isNaValue(SrsiOut[0]));
  EXPECT_TRUE(dsl::isNaValue(SrsiOut[1]));
  EXPECT_TRUE(dsl::isNaValue(SrsiOut[2]));
  // bar 2 rsi=200/3, bar 3 rsi=800/9 -> stoch_rsi(bar 3) = 100
  EXPECT_NEAR(SrsiOut[3], 100.0, 1e-9);
  EXPECT_NEAR(SrsiOut[4], 0.0, 1e-9);
}

TEST(IndicatorsTest, TaDirectionalUltOscAndElderRay) {
  // Strictly rising bars -> +DI = 100, -DI = 0, DX = 100, ADX = 100
  const std::vector<dsl::Bar> UpBars = {
      makeBar(0, 10.0, 10.0, 10.0, 10.0),
      makeBar(1, 12.0, 11.0, 12.0, 11.0),
      makeBar(2, 14.0, 13.0, 14.0, 13.0),
      makeBar(3, 16.0, 15.0, 16.0, 15.0),
      makeBar(4, 18.0, 17.0, 18.0, 17.0),
  };
  const auto PlusDi = probeSignedExpr("plus_di(2)", UpBars);
  const auto MinusDi = probeSignedExpr("minus_di(2)", UpBars);
  const auto AdxOut = probeSignedExpr("adx(2)", UpBars);
  EXPECT_TRUE(dsl::isNaValue(PlusDi[0]));
  EXPECT_TRUE(dsl::isNaValue(PlusDi[1]));
  EXPECT_NEAR(PlusDi[2], 100.0, 1e-9);
  EXPECT_NEAR(MinusDi[2], 0.0, 1e-9);
  // ADX(2) warms up at bar 2*2 - 1 = 3 (4th bar)
  EXPECT_TRUE(dsl::isNaValue(AdxOut[2]));
  EXPECT_NEAR(AdxOut[3], 100.0, 1e-9);
  EXPECT_NEAR(AdxOut[4], 100.0, 1e-9);

  // Ultimate Oscillator(1, 2, 3) on bars closing at their high:
  // bp == tr on every bar -> avg1=avg2=avg3=1 -> UO = 100
  const auto UoOut = probeSignedExpr("ult_osc(1, 2, 3)", UpBars);
  EXPECT_TRUE(dsl::isNaValue(UoOut[0]));
  EXPECT_TRUE(dsl::isNaValue(UoOut[1]));
  EXPECT_TRUE(dsl::isNaValue(UoOut[2]));
  EXPECT_NEAR(UoOut[3], 100.0, 1e-9);

  // Elder-Ray Bull/Bear Power(2) on {10, 12, 14}:
  // ema(2) at bar 1 = 11 -> bull = high(12)-11 = 1, bear = low(11)-11 = 0
  const auto BullOut = probeSignedExpr("bull_power(2)", UpBars);
  const auto BearOut = probeSignedExpr("bear_power(2)", UpBars);
  EXPECT_TRUE(dsl::isNaValue(BullOut[0]));
  EXPECT_NEAR(BullOut[1], 1.0, 1e-9);
  EXPECT_NEAR(BearOut[1], 0.0, 1e-9);
}

TEST(IndicatorsTest, TaVolumeObvAndMfi) {
  const std::vector<dsl::Bar> VolBars = {
      makeBar(0, 10.0, 10.0, 10.0, 10.0, 100.0),
      makeBar(1, 12.0, 10.0, 12.0, 10.0, 200.0),
      makeBar(2, 12.0, 12.0, 12.0, 12.0, 300.0),
      makeBar(3, 11.0, 12.0, 12.0, 11.0, 150.0),
  };
  const auto ObvOut = probeSignedExpr("obv()", VolBars);
  ASSERT_EQ(ObvOut.size(), 4U);
  EXPECT_NEAR(ObvOut[0], 0.0, 1e-9);
  EXPECT_NEAR(ObvOut[1], 200.0, 1e-9);
  EXPECT_NEAR(ObvOut[2], 200.0, 1e-9);
  EXPECT_NEAR(ObvOut[3], 50.0, 1e-9);

  // MFI(2):
  // bar 0: tp=10
  // bar 1: tp=34/3 > 10 -> posMf = (34/3)*200
  // bar 2: tp=12 > 34/3 -> posMf = 12*300 -> both positive -> MFI = 100
  // bar 3: tp=34/3 < 12 -> negMf = (34/3)*150 = 1700; window {bar2 pos=3600,
  //        bar3 neg=1700} -> 100 * 3600 / (3600 + 1700)
  const auto MfiOut = probeSignedExpr("mfi(2)", VolBars);
  EXPECT_TRUE(dsl::isNaValue(MfiOut[0]));
  EXPECT_TRUE(dsl::isNaValue(MfiOut[1]));
  EXPECT_NEAR(MfiOut[2], 100.0, 1e-9);
  EXPECT_NEAR(MfiOut[3], 100.0 * 3600.0 / 5300.0, 1e-9);
}

TEST(IndicatorsTest, TaIndicatorsDeterminismAndOffsets) {
  const std::string Src = R"dsl(
rule "det"
  when not is_na(adx(3)) and not is_na(macd_hist(close, 2, 3, 2))
       and not is_na(stoch_rsi(3)) and not is_na(ult_osc(2, 3, 4))
       and not is_na(mfi(3)) and not is_na(bb_upper(close, 3, 2))
       and not is_na(vwap(3)) and not is_na(obv()[1])
  then buy 1 lots
  because "w={wma(close, 3)}; m={macd_hist(close, 2, 3, 2)}; a={adx(3)}; o={obv()}; c={cci(3)}"
)dsl";
  auto CompA = dsl::compile(Src, {}, 1);
  auto CompB = dsl::compile(Src, {}, 1);
  ASSERT_TRUE(CompA.Ok);
  ASSERT_TRUE(CompB.Ok);

  dsl::PortfolioSnapshot Port;
  Port.Cash = 100'000.0;
  Port.Equity = 100'000.0;
  bool SawFired = false;
  for (int StepIdx = 0; StepIdx < 15; ++StepIdx) {
    const double Base = 100.0 + static_cast<double>((StepIdx * 7) % 11);
    const auto BarNow = makeBar(StepIdx, Base, Base - 0.5, Base + 2.0,
                                Base - 1.5, 100.0 + StepIdx * 10.0);
    const auto ResA = dsl::step(*CompA.Instance, 0, BarNow, Port, false);
    const auto ResB = dsl::step(*CompB.Instance, 0, BarNow, Port, false);
    ASSERT_FALSE(ResA.Error.has_value());
    ASSERT_FALSE(ResB.Error.has_value());
    EXPECT_EQ(ResA.Intents, ResB.Intents);
    if (!ResA.Intents.empty()) {
      SawFired = true;
    }
  }
  EXPECT_TRUE(SawFired);
}

} // namespace
