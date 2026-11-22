#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstddef>
#include <limits>

#include "dsl/dsl.hpp"

namespace {

TEST(Version, MatchesConstantAndIsNotEmpty) {
  EXPECT_FALSE(dsl::version().empty());
  EXPECT_EQ(dsl::version(), dsl::kVersion);
  EXPECT_EQ(dsl::version(), "0.1.0");
}

TEST(Types, NaHelperAndDefaults) {
  EXPECT_TRUE(dsl::isNaValue(dsl::kNa));
  EXPECT_TRUE(dsl::isNaValue(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_FALSE(dsl::isNaValue(0.0));
  EXPECT_FALSE(dsl::isNaValue(1.0));
  EXPECT_FALSE(dsl::isNaValue(std::numeric_limits<double>::infinity()));

  const dsl::SourcePos ZeroPos;
  const dsl::SourcePos NextPos{1, 2};
  EXPECT_EQ(ZeroPos.Line, 0U);
  EXPECT_EQ(ZeroPos.Column, 0U);
  EXPECT_LT(ZeroPos, NextPos);

  const dsl::SourceRange EmptyRange;
  EXPECT_EQ(EmptyRange.Start, ZeroPos);
  EXPECT_EQ(EmptyRange.End, ZeroPos);

  const dsl::Diagnostic DefaultDiag;
  EXPECT_EQ(DefaultDiag.Severity, dsl::DiagnosticSeverity::Error);
  EXPECT_TRUE(DefaultDiag.Fixes.empty());

  const dsl::CheckResult DefaultCheck;
  EXPECT_EQ(DefaultCheck.Lookahead, dsl::LookaheadLevel::Ok);
  EXPECT_TRUE(DefaultCheck.Diagnostics.empty());
  EXPECT_TRUE(DefaultCheck.Params.empty());

  const dsl::PortfolioSnapshot DefaultPortfolio;
  EXPECT_EQ(DefaultPortfolio.Position, 0);
  EXPECT_TRUE(dsl::isNaValue(DefaultPortfolio.EntryPrice));
  EXPECT_FALSE(DefaultPortfolio.BarsInPosition.has_value());
  EXPECT_EQ(DefaultPortfolio.LotSize, 1);
  EXPECT_FALSE(DefaultPortfolio.PositionReopenedOnBar);

  const dsl::RuleIntent DefaultIntent;
  EXPECT_EQ(DefaultIntent.Kind, dsl::ActionKind::Buy);
  EXPECT_EQ(DefaultIntent.OrderKind, dsl::OrderType::Market);
  EXPECT_EQ(DefaultIntent.Tif, dsl::TimeInForce::Gtc);
  EXPECT_TRUE(dsl::isNaValue(DefaultIntent.RawPrice));

  const dsl::StepOutcome DefaultOutcome;
  EXPECT_TRUE(DefaultOutcome.Intents.empty());
  EXPECT_FALSE(DefaultOutcome.Error.has_value());
}

TEST(Types, LimitsMatchSpecification) {
  EXPECT_EQ(dsl::kMaxSourceBytes, 65536U);
  EXPECT_EQ(dsl::kMaxRules, 100U);
  EXPECT_EQ(dsl::kMaxParams, 50U);
  EXPECT_EQ(dsl::kMaxExprDepth, 100U);
  EXPECT_EQ(dsl::kMaxAstNodes, 2000U);
  EXPECT_EQ(dsl::kMaxDagNodes, 256U);
  EXPECT_EQ(dsl::kMaxLookback, 5000U);
  EXPECT_EQ(dsl::kMaxSyntaxErrors, 20U);
  EXPECT_EQ(dsl::kMaxOrderLots, 1'000'000'000'000LL);
  EXPECT_DOUBLE_EQ(dsl::kMaxDecimalPrice, 1e15);
}

RC_GTEST_PROP(TypesProperty, SourcePosOrderingIsAntisymmetric,
              (std::size_t LineA, std::size_t ColA, std::size_t LineB,
               std::size_t ColB)) {
  const dsl::SourcePos PosA{LineA, ColA};
  const dsl::SourcePos PosB{LineB, ColB};
  if (PosA < PosB) {
    RC_ASSERT(PosB > PosA);
    RC_ASSERT(!(PosA == PosB));
  } else if (PosA == PosB) {
    RC_ASSERT(!(PosA < PosB));
    RC_ASSERT(!(PosB < PosA));
  } else {
    RC_ASSERT(PosB < PosA);
  }
}

} // namespace
