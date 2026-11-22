#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <string_view>
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

std::size_t countCode(const std::vector<dsl::Diagnostic> &Diags,
                      std::string_view Code) {
  std::size_t Count = 0;
  for (const auto &Diag : Diags) {
    if (Diag.Code == Code) {
      ++Count;
    }
  }
  return Count;
}

bool hasCode(const std::vector<dsl::Diagnostic> &Diags, std::string_view Code) {
  return countCode(Diags, Code) > 0;
}

std::vector<dsl::TextEdit>
collectEdits(const std::vector<dsl::Diagnostic> &Diags, std::string_view Code) {
  std::vector<dsl::TextEdit> Edits;
  for (const auto &Diag : Diags) {
    if (Diag.Code != Code) {
      continue;
    }
    for (const auto &Fix : Diag.Fixes) {
      Edits.insert(Edits.end(), Fix.Edits.begin(), Fix.Edits.end());
    }
  }
  return Edits;
}

TEST(CheckTest, PlatformStandardPassesCleanly) {
  const auto Res = dsl::check(kPlatformStandard);
  EXPECT_TRUE(Res.Diagnostics.empty());
  EXPECT_EQ(Res.Lookahead, dsl::LookaheadLevel::Ok);
  ASSERT_EQ(Res.Params.size(), 6U);

  EXPECT_EQ(Res.Params[0].Name, "fast");
  EXPECT_DOUBLE_EQ(Res.Params[0].DefaultValue, 20.0);
  EXPECT_EQ(Res.Params[0].RangeMin, 5.0);
  EXPECT_EQ(Res.Params[0].RangeMax, 50.0);
  EXPECT_EQ(Res.Params[0].StepText, "5");
  EXPECT_TRUE(Res.Params[0].IsInteger);

  EXPECT_EQ(Res.Params[1].Name, "slow");
  EXPECT_EQ(Res.Params[1].StepText, "10");
  EXPECT_TRUE(Res.Params[1].IsInteger);

  EXPECT_EQ(Res.Params[2].Name, "rsi_n");
  EXPECT_FALSE(Res.Params[2].StepText.has_value());
  EXPECT_TRUE(Res.Params[2].IsInteger);

  EXPECT_EQ(Res.Params[3].Name, "rsi_high");
  EXPECT_EQ(Res.Params[3].StepText, "5");
  EXPECT_FALSE(Res.Params[3].IsInteger);

  EXPECT_EQ(Res.Params[4].Name, "stop_pct");
  EXPECT_EQ(Res.Params[4].StepText, "1");
  EXPECT_FALSE(Res.Params[4].IsInteger);

  EXPECT_EQ(Res.Params[5].Name, "size_pct");
  EXPECT_FALSE(Res.Params[5].StepText.has_value());
  EXPECT_FALSE(Res.Params[5].IsInteger);
}

TEST(CheckTest, Utf8ValidationAndSizeLimit) {
  const auto BadUtf8 = dsl::check("\xff\xfe");
  ASSERT_EQ(BadUtf8.Diagnostics.size(), 1U);
  EXPECT_EQ(BadUtf8.Diagnostics[0].Code, "E-SYN");
  EXPECT_EQ(BadUtf8.Diagnostics[0].Range.Start, (dsl::SourcePos{0, 0}));
  EXPECT_EQ(BadUtf8.Diagnostics[0].Range.End, (dsl::SourcePos{0, 0}));
  EXPECT_TRUE(BadUtf8.Params.empty());

  const std::string TooBig(dsl::kMaxSourceBytes + 1U, ' ');
  const auto BigRes = dsl::check(TooBig);
  ASSERT_EQ(BigRes.Diagnostics.size(), 1U);
  EXPECT_EQ(BigRes.Diagnostics[0].Code, "E-SYN");
  EXPECT_EQ(BigRes.Diagnostics[0].Range.Start, (dsl::SourcePos{0, 0}));
  EXPECT_EQ(BigRes.Diagnostics[0].Range.End, (dsl::SourcePos{0, 0}));

  const std::string RuleText =
      "rule \"ok\" when true then cancel because \"ok\"";
  std::string ExactLimit = RuleText;
  ExactLimit.append(dsl::kMaxSourceBytes - RuleText.size(), ' ');
  const auto OkLimit = dsl::check(ExactLimit);
  EXPECT_EQ(countCode(OkLimit.Diagnostics, "E-SYN"), 0U);
}

TEST(CheckTest, UnicodeCoordinatesAndLineEndings) {
  const std::string Src =
      "rule \"абв\" when close > 0 then cancel because \"причина\"\r\n"
      "rule \"где\" when close > x then cancel because \"ок\"\r"
      "rule \"ёж\" when bad > 0 then cancel because \"ок\"\n";
  const auto Res = dsl::check(Src);
  EXPECT_EQ(countCode(Res.Diagnostics, "W-LA01"), 1U);
  ASSERT_EQ(countCode(Res.Diagnostics, "E-TYPE"), 2U);

  std::vector<dsl::Diagnostic> TypeDiags;
  for (const auto &Diag : Res.Diagnostics) {
    if (Diag.Code == "E-TYPE") {
      TypeDiags.push_back(Diag);
    }
  }
  EXPECT_EQ(TypeDiags[0].Range.Start, (dsl::SourcePos{2, 25}));
  EXPECT_EQ(TypeDiags[0].Range.End, (dsl::SourcePos{2, 26}));
  EXPECT_EQ(TypeDiags[1].Range.Start, (dsl::SourcePos{3, 16}));
  EXPECT_EQ(TypeDiags[1].Range.End, (dsl::SourcePos{3, 19}));
}

TEST(CheckTest, LexerNumbersStringsAndBecause) {
  const std::string Valid =
      "param p = 20 range 5..50 step 0.5\n"
      "rule \"a\\\"b\\\\c\" when close[5] > p then cancel\n"
      "  because \"{{x}} {close + 1} }}\"\n";
  const auto ValidRes = dsl::check(Valid);
  EXPECT_EQ(countCode(ValidRes.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(ValidRes.Diagnostics, "E-TYPE"), 0U);

  const char *const BadCases[] = {
      "param p = 1e400\nrule \"r\" when true then cancel because \"x\"",
      "rule \"a\tb\" when true then cancel because \"x\"",
      "rule \"a\\nb\" when true then cancel because \"x\"",
      "rule \"a\nb\" when true then cancel because \"x\"",
      "rule \"r\" when true then cancel because \"{close # c}\"",
      "rule \"r\" when true then cancel because \"{}\"",
      "rule \"r\" when true then cancel because \"{   }\"",
      "rule \"r\" when true then cancel because \"{\"",
      "rule \"r\" when true then cancel because \"}\"",
      "rule \"r\" when true then cancel because \"{{}\"",
      "rule \"r\" when true then cancel because \"{a + {b}}\"",
  };
  for (const char *Case : BadCases) {
    EXPECT_TRUE(hasCode(dsl::check(Case).Diagnostics, "E-SYN")) << Case;
  }
}

TEST(CheckTest, GrammarNonChainingRecoveryAndLimits) {
  const char *const NonChaining[] = {
      "rule \"r\" when --close > 0 then cancel because \"x\"",
      "rule \"r\" when not not true then cancel because \"x\"",
      "rule \"r\" when 1 < close < 10 then cancel because \"x\"",
      "rule \"r\" when close[1][2] > 0 then cancel because \"x\"",
  };
  for (const char *Case : NonChaining) {
    EXPECT_TRUE(hasCode(dsl::check(Case).Diagnostics, "E-SYN")) << Case;
  }

  const std::string Parenthesized =
      "rule \"r\" when not (not (1 < close)) and -(-close) > 0\n"
      "  then cancel because \"x\"";
  EXPECT_EQ(countCode(dsl::check(Parenthesized).Diagnostics, "E-SYN"), 0U);

  std::string ManyErrors;
  for (std::size_t Idx = 0; Idx < 25U; ++Idx) {
    ManyErrors += "rule \"r" + std::to_string(Idx) + "\" when ) then cancel\n";
  }
  const auto CapRes = dsl::check(ManyErrors);
  EXPECT_EQ(countCode(CapRes.Diagnostics, "E-SYN"), dsl::kMaxSyntaxErrors);

  std::string TooManyParams;
  for (std::size_t Idx = 0; Idx <= dsl::kMaxParams; ++Idx) {
    TooManyParams += "param p" + std::to_string(Idx) + " = 1\n";
  }
  TooManyParams += "rule \"r\" when true then cancel because \"x\"";
  EXPECT_TRUE(hasCode(dsl::check(TooManyParams).Diagnostics, "E-SYN"));

  std::string TooManyRules;
  for (std::size_t Idx = 0; Idx <= dsl::kMaxRules; ++Idx) {
    TooManyRules += "rule \"r" + std::to_string(Idx) +
                    "\" when true then cancel because \"x\"\n";
  }
  EXPECT_TRUE(hasCode(dsl::check(TooManyRules).Diagnostics, "E-SYN"));

  std::string DeepExpr;
  for (std::size_t Idx = 0; Idx < 101U; ++Idx) {
    DeepExpr += "abs(";
  }
  DeepExpr += "close";
  DeepExpr.append(101U, ')');
  const std::string DeepSrc =
      "rule \"r\" when " + DeepExpr + " > 0 then cancel because \"x\"";
  EXPECT_TRUE(hasCode(dsl::check(DeepSrc).Diagnostics, "E-SYN"));

  std::string ManyNodes = "rule \"r\" when 0";
  for (std::size_t Idx = 0; Idx < 1100U; ++Idx) {
    ManyNodes += " + 1";
  }
  ManyNodes += " > 0 then cancel because \"x\"";
  EXPECT_TRUE(hasCode(dsl::check(ManyNodes).Diagnostics, "E-SYN"));

  std::string ManyDag = "rule \"r\" when ";
  for (std::size_t Idx = 1; Idx <= 257U; ++Idx) {
    if (Idx > 1U) {
      ManyDag += " + ";
    }
    ManyDag += "sma(close, " + std::to_string(Idx) + ")";
  }
  ManyDag += " > 0 then cancel because \"x\"";
  EXPECT_TRUE(hasCode(dsl::check(ManyDag).Diagnostics, "E-SYN"));
}

TEST(CheckTest, MissingAndEmptyBecause) {
  const std::string MissingBeforeRule =
      "rule \"a\" when true then cancel\n"
      "rule \"b\" when true then cancel because \"ok\"\n";
  const auto Res1 = dsl::check(MissingBeforeRule);
  EXPECT_EQ(countCode(Res1.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(Res1.Diagnostics, "E-BEC"), 1U);

  const std::string MissingAtEof = "rule \"a\" when true then cancel";
  const auto Res2 = dsl::check(MissingAtEof);
  EXPECT_EQ(countCode(Res2.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(Res2.Diagnostics, "E-BEC"), 1U);

  const std::string EmptyBecause =
      "rule \"a\" when true then cancel because \"\"\n"
      "rule \"b\" when true then cancel because \" \xC2\xA0 \"\n";
  const auto Res3 = dsl::check(EmptyBecause);
  EXPECT_EQ(countCode(Res3.Diagnostics, "E-BEC"), 2U);

  const std::string InterpOnly =
      "rule \"a\" when true then cancel because \"{close}\"";
  EXPECT_EQ(countCode(dsl::check(InterpOnly).Diagnostics, "E-BEC"), 0U);
}

TEST(CheckTest, Phase2SemanticAndTypeErrors) {
  // Empty and duplicate rule names (including trimming spaces & U+00A0).
  const std::string DupAndEmptyRules =
      "rule \" \xC2\xA0 \" when true then cancel because \"x\"\n"
      "rule \"r\" when true then cancel because \"x\"\n"
      "rule \" r \" when true then cancel because \"x\"\n";
  EXPECT_EQ(countCode(dsl::check(DupAndEmptyRules).Diagnostics, "E-TYPE"), 2U);

  const std::string ParamErrs =
      "param p = 1\n"
      "param p = 2\n"
      "param close = 1\n"
      "param bad_rng = 10 range 1..5 step 1\n"
      "param bad_step = 3 range 1..5 step 0\n"
      "rule \"r\" when true then cancel because \"x\"";
  EXPECT_EQ(countCode(dsl::check(ParamErrs).Diagnostics, "E-TYPE"), 4U);

  const std::string OvSrc =
      "param n = 10 range 5..20 step 5\n"
      "rule \"r\" when sma(close, n) > 0 then cancel because \"x\"";
  const auto OvUnknown = dsl::check(OvSrc, {{"missing", 1.0}});
  ASSERT_EQ(countCode(OvUnknown.Diagnostics, "E-TYPE"), 1U);
  EXPECT_EQ(OvUnknown.Diagnostics[0].Range.Start, (dsl::SourcePos{0, 0}));

  const auto OvRange = dsl::check(OvSrc, {{"n", 25.0}});
  EXPECT_EQ(countCode(OvRange.Diagnostics, "E-TYPE"), 1U);
  const auto OvNonInt = dsl::check(OvSrc, {{"n", 10.5}});
  EXPECT_EQ(countCode(OvNonInt.Diagnostics, "E-TYPE"), 1U);
  const auto OvNan =
      dsl::check(OvSrc, {{"n", std::numeric_limits<double>::quiet_NaN()}});
  EXPECT_EQ(countCode(OvNan.Diagnostics, "E-TYPE"), 1U);

  const char *const TypeErrorCases[] = {
      "rule \"r\" when sma(close) > 0 then cancel because \"x\"",
      "rule \"r\" when foo(close) > 0 then cancel because \"x\"",
      "rule \"r\" when sma(equity, 10) > 0 then cancel because \"x\"",
      "rule \"r\" when position[1] > 0 then cancel because \"x\"",
      "rule \"r\" when close[5001] > 0 then cancel because \"x\"",
      "rule \"r\" when sma(close, 5001) > 0 then cancel because \"x\"",
      "rule \"r\" when true then sell 50 % equity because \"x\"",
      "rule \"r\" when true then sell 50 % cash because \"x\"",
      "rule \"r\" when true then buy 1 lots take_profit 100 because \"x\"",
      "rule \"r\" when true then buy all because \"x\"",
      "rule \"r\" when true or close then cancel because \"x\"",
      "rule \"r\" when close then cancel because \"x\"",
  };
  for (const char *Case : TypeErrorCases) {
    const auto Res = dsl::check(Case);
    EXPECT_TRUE(hasCode(Res.Diagnostics, "E-TYPE")) << Case;
  }

  const std::string BadNegParam =
      "param p = 1.5\n"
      "rule \"r\" when close[-p] > 0 then cancel because \"x\"";
  const auto NegParamRes = dsl::check(BadNegParam);
  EXPECT_EQ(countCode(NegParamRes.Diagnostics, "E-TYPE"), 1U);
  EXPECT_EQ(countCode(NegParamRes.Diagnostics, "E-LA01"), 0U);
  EXPECT_EQ(countCode(NegParamRes.Diagnostics, "E-LA02"), 0U);
}

TEST(CheckTest, Phase3LookaheadDetection) {
  const auto ElaLit = dsl::check(
      "rule \"r\" when close[-1] > close then buy 1 lots because \"x\"");
  EXPECT_EQ(countCode(ElaLit.Diagnostics, "E-LA01"), 1U);
  EXPECT_EQ(ElaLit.Lookahead, dsl::LookaheadLevel::Error);

  const std::string ZeroSrc =
      "rule \"r\" when close[-0] > 0 then cancel because \"x\"";
  const auto ElaZero = dsl::check(ZeroSrc);
  EXPECT_EQ(countCode(ElaZero.Diagnostics, "E-LA01"), 0U);
  EXPECT_EQ(ElaZero.Lookahead, dsl::LookaheadLevel::Warning);

  const auto ElaParam = dsl::check(
      "param p = -1\nrule \"r\" when close[p] > 0 then cancel because \"x\"");
  EXPECT_EQ(countCode(ElaParam.Diagnostics, "E-LA01"), 1U);
  EXPECT_EQ(ElaParam.Lookahead, dsl::LookaheadLevel::Error);

  const std::string RangeSrc =
      "param p = 1 range -5..5 step 1\n"
      "rule \"r\" when close[p] > 0 then cancel because \"x\"";
  const auto ElaRange = dsl::check(RangeSrc);
  EXPECT_EQ(countCode(ElaRange.Diagnostics, "E-LA02"), 1U);
  EXPECT_EQ(ElaRange.Lookahead, dsl::LookaheadLevel::Error);

  const std::string NegRangeSrc =
      "param p = 0 range -5..0 step 1\n"
      "rule \"r\" when close[-p] > 0 then cancel because \"x\"";
  const auto OkNegRange = dsl::check(NegRangeSrc);
  EXPECT_EQ(countCode(OkNegRange.Diagnostics, "E-LA01"), 0U);
  EXPECT_EQ(OkNegRange.Lookahead, dsl::LookaheadLevel::Warning);

  const auto WlaSma = dsl::check(
      "param n = 10\n"
      "rule \"r\" when sma(close, n) > 250 then cancel because \"x\"");
  EXPECT_EQ(countCode(WlaSma.Diagnostics, "W-LA01"), 1U);
  EXPECT_EQ(WlaSma.Lookahead, dsl::LookaheadLevel::Warning);

  const std::string NestedSrc =
      "param a = 10\nparam b = 20\nparam lvl = 250\n"
      "rule \"r\" when ema(sma(close, a), b) > lvl then cancel\n"
      "  because \"x\"";
  const auto WlaNested = dsl::check(NestedSrc);
  EXPECT_EQ(countCode(WlaNested.Diagnostics, "W-LA01"), 0U);
  EXPECT_EQ(WlaNested.Lookahead, dsl::LookaheadLevel::Ok);

  const auto WlaSmaOffset1 = dsl::check(
      "rule \"r\" when sma(close, 20)[1] > 250 then cancel because \"x\"");
  EXPECT_EQ(countCode(WlaSmaOffset1.Diagnostics, "W-LA01"), 1U);
  EXPECT_EQ(WlaSmaOffset1.Lookahead, dsl::LookaheadLevel::Warning);

  const auto WlaSmaOffset0 = dsl::check(
      "rule \"r\" when sma(close, 20)[0] > 250 then cancel because \"x\"");
  EXPECT_EQ(countCode(WlaSmaOffset0.Diagnostics, "W-LA01"), 1U);
  EXPECT_EQ(WlaSmaOffset0.Lookahead, dsl::LookaheadLevel::Warning);

  const std::string NestedOffsetSrc =
      "param a = 10\nparam b = 20\nparam lvl = 250\n"
      "rule \"r\" when ema(sma(close, a), b)[1] > lvl then cancel\n"
      "  because \"x\"";
  const auto WlaNestedOffset = dsl::check(NestedOffsetSrc);
  EXPECT_EQ(countCode(WlaNestedOffset.Diagnostics, "W-LA01"), 0U);
  EXPECT_EQ(WlaNestedOffset.Lookahead, dsl::LookaheadLevel::Ok);

  const std::string HugeNeg =
      "param p = -100000000000000000000\n"
      "rule \"r\" when sma(close[p], 10) > 0 then cancel because \"x\"";
  const auto HugeNegRes = dsl::check(HugeNeg);
  EXPECT_EQ(countCode(HugeNegRes.Diagnostics, "E-LA01"), 1U);
  EXPECT_EQ(HugeNegRes.Lookahead, dsl::LookaheadLevel::Error);
}

TEST(CheckTest, Phase4LinterAndAutoFixes) {
  const std::string Skipped =
      "rule \"r\" when close > 0 and close[1] > 1.0 and 100.0 > 1\n"
      "  then buy 100 lots because \"x\"";
  EXPECT_EQ(countCode(dsl::check(Skipped).Diagnostics, "L001"), 0U);

  const std::string NegOffset =
      "rule \"r\" when close[-2] > 0 then cancel because \"x\"";
  EXPECT_EQ(countCode(dsl::check(NegOffset).Diagnostics, "L001"), 0U);

  const std::string LinterSrc =
      "# leading comment\n\n"
      "rule \"r\" when change(close, 5) < -2.5 and close[3] > 1 - 0.05\n"
      "  then buy 10 lots limit 150\n"
      "  because \"v={close + 7}\"\n";
  const auto Res = dsl::check(LinterSrc);
  EXPECT_EQ(countCode(Res.Diagnostics, "L001"), 7U);

  const auto Edits = collectEdits(Res.Diagnostics, "L001");
  ASSERT_FALSE(Edits.empty());
  const std::string Fixed = dsl::applyEdits(LinterSrc, Edits);
  EXPECT_EQ(Fixed.rfind("# leading comment\n\nparam ", 0), 0U);
  EXPECT_NE(Fixed.find("param change_n = 5"), std::string::npos);
  EXPECT_NE(Fixed.find("param change_level = -2.5"), std::string::npos);

  const auto AfterFix = dsl::check(Fixed);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "E-TYPE"), 0U);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "L001"), 0U);

  const std::string Collide =
      "param sma_n = 10\n"
      "rule \"r\" when sma(close, 20) > sma(close, 30)\n"
      "  then cancel because \"x\"";
  const std::string FixedCollide = dsl::applyEdits(
      Collide, collectEdits(dsl::check(Collide).Diagnostics, "L001"));
  EXPECT_NE(FixedCollide.find("param sma_n_2 = 20"), std::string::npos);
  EXPECT_NE(FixedCollide.find("param sma_n_3 = 30"), std::string::npos);

  std::string Params49;
  for (std::size_t Idx = 0; Idx < 49U; ++Idx) {
    Params49 += "param k" + std::to_string(Idx) + " = 1\n";
  }
  const auto At49 = dsl::check(
      Params49 + "rule \"r\" when close > 5 and open > 6 then cancel\n"
                 "  because \"x\"");
  std::size_t WithFixes = 0;
  std::size_t WithoutFixes = 0;
  for (const auto &Diag : At49.Diagnostics) {
    if (Diag.Code == "L001") {
      if (Diag.Fixes.empty()) {
        ++WithoutFixes;
      } else {
        ++WithFixes;
      }
    }
  }
  EXPECT_EQ(WithFixes, 1U);
  EXPECT_EQ(WithoutFixes, 1U);

  const auto At50 = dsl::check(
      Params49 + "param k49 = 1\n"
                 "rule \"r\" when close > 5 then cancel because \"x\"");
  ASSERT_EQ(countCode(At50.Diagnostics, "L001"), 1U);
  for (const auto &Diag : At50.Diagnostics) {
    if (Diag.Code == "L001") {
      EXPECT_TRUE(Diag.Fixes.empty());
    }
  }
}

TEST(CheckTest, RoundTripPrintParseAndDeterminism) {
  const auto ParsedStd = dsl::parse(kPlatformStandard);
  ASSERT_FALSE(ParsedStd.HasSyntaxErrors);
  const std::string PrintedStd = dsl::print(ParsedStd.Ast);
  const auto ReparsedStd = dsl::parse(PrintedStd);
  ASSERT_FALSE(ReparsedStd.HasSyntaxErrors);
  EXPECT_TRUE(dsl::astEqual(ParsedStd.Ast, ReparsedStd.Ast));

  const std::string ComplexSrc =
      "param a = -2 range -10..10 step 0.5\n"
      "rule \"q\\\"w\\\\e\" when not (not (a < 0)) and -(-close[1]) > 0\n"
      "  then buy 10 lots market gtc\n"
      "  because \"{{x}} {sma(close, 5)[1] + 1} \\\"ok\\\" \\\\\"\n"
      "rule \"c\" when false then cancel because \"done\"\n";
  const auto ParsedCmp = dsl::parse(ComplexSrc);
  ASSERT_FALSE(ParsedCmp.HasSyntaxErrors);
  const auto ReparsedCmp = dsl::parse(dsl::print(ParsedCmp.Ast));
  ASSERT_FALSE(ReparsedCmp.HasSyntaxErrors);
  EXPECT_TRUE(dsl::astEqual(ParsedCmp.Ast, ReparsedCmp.Ast));

  const std::string MultiErr =
      "param x = 1\nparam x = 2\n"
      "rule \"r\" when close[-1] > 200 and bad > 0 then buy all\n";
  const auto Run1 = dsl::check(MultiErr);
  const auto Run2 = dsl::check(MultiErr);
  EXPECT_EQ(Run1.Diagnostics, Run2.Diagnostics);
  for (std::size_t Idx = 1; Idx < Run1.Diagnostics.size(); ++Idx) {
    const auto &Prev = Run1.Diagnostics[Idx - 1];
    const auto &Curr = Run1.Diagnostics[Idx];
    const bool Ordered =
        (Prev.Range.Start < Curr.Range.Start) ||
        (Prev.Range.Start == Curr.Range.Start &&
         Prev.Range.End < Curr.Range.End) ||
        (Prev.Range.Start == Curr.Range.Start &&
         Prev.Range.End == Curr.Range.End && Prev.Code <= Curr.Code);
    EXPECT_TRUE(Ordered);
  }
}

TEST(CheckTest, L001ParenthesizedLiteralsPreserveOuterParens) {
  const std::string Src = "rule \"r\"\n"
                          "  when(5)>close and(2.5)>1\n"
                          "  then buy(5)lots limit(100.5)gtc\n"
                          "  because \"ok\"\n"
                          "rule \"neg\"\n"
                          "  when(-7)<close\n"
                          "  then buy(-3)lots\n"
                          "  because \"neg\"\n";
  const auto Res = dsl::check(Src);
  EXPECT_EQ(countCode(Res.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(Res.Diagnostics, "L001"), 6U);

  const auto Edits = collectEdits(Res.Diagnostics, "L001");
  ASSERT_FALSE(Edits.empty());
  const std::string Fixed = dsl::applyEdits(Src, Edits);
  EXPECT_NE(Fixed.find("when(p1)>close"), std::string::npos);
  EXPECT_NE(Fixed.find("and(p2)>1"), std::string::npos);
  EXPECT_NE(Fixed.find("buy(p3)lots"), std::string::npos);
  EXPECT_NE(Fixed.find("limit(p4)gtc"), std::string::npos);
  EXPECT_NE(Fixed.find("when(p5)<close"), std::string::npos);
  EXPECT_NE(Fixed.find("buy(p6)lots"), std::string::npos);

  const auto AfterFix = dsl::check(Fixed);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "E-TYPE"), 0U);
  EXPECT_EQ(countCode(AfterFix.Diagnostics, "L001"), 0U);
}

TEST(CheckTest, DagZeroOffsetCanonicalizationAt256Limit) {
  std::string Src = "param p = 0\n";
  for (std::size_t RuleIdx = 0; RuleIdx < 8U; ++RuleIdx) {
    Src += "rule \"r" + std::to_string(RuleIdx) + "\" when ";
    for (std::size_t Term = 0; Term < 32U; ++Term) {
      const std::size_t Period = RuleIdx * 32U + Term + 1U;
      if (Term > 0U) {
        Src += " + ";
      }
      Src += "sma(close, " + std::to_string(Period) + ")";
    }
    Src += " > 0 then cancel because \"x\"\n";
  }
  Src += "rule \"dup_zero\" when sma(close[0], 10) > 0 and "
         "sma(close[p], 10) > 0 and sma(close[-0], 10) > 0 "
         "then cancel because \"x\"\n";

  const auto CheckOk = dsl::check(Src);
  EXPECT_EQ(countCode(CheckOk.Diagnostics, "E-SYN"), 0U);
  EXPECT_EQ(countCode(CheckOk.Diagnostics, "E-TYPE"), 0U);

  const auto Compiled = dsl::compile(Src, {}, 1);
  ASSERT_TRUE(Compiled.Ok);
  ASSERT_NE(Compiled.Instance, nullptr);
  EXPECT_EQ(Compiled.Instance->dagNodeCount(), dsl::kMaxDagNodes);

  const std::string ExtraRule =
      "rule \"extra\" when sma(close[1], 10) > 0\n"
      "  then cancel because \"x\"\n";
  const std::string OverLimitSrc = Src + ExtraRule;
  const auto CheckErr = dsl::check(OverLimitSrc);
  EXPECT_EQ(countCode(CheckErr.Diagnostics, "E-SYN"), 1U);
}

} // namespace
