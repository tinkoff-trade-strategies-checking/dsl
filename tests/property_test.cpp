#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dsl/dsl.hpp"

namespace dsl {
namespace {

constexpr std::size_t kMaxGenDepth = 2;

struct ParamSpec {
  std::string Name;
  bool IsInt = false;
  bool CanUseAsPeriod = false;
  bool CanUseAsOffset = false;
};

struct GenConfig {
  std::size_t MinParams = 0;
  std::size_t MaxParams = 10;
  std::size_t MinRules = 1;
  std::size_t MaxRules = 6;
  std::size_t MaxMagic = 12;
  bool AllowMagic = true;
  bool RequireMagic = false;
  bool AllowComments = true;
  bool AllowNonNegParamOffsets = true;
};

struct GenContext {
  std::vector<ParamSpec> Params;
  std::vector<std::string> PeriodParams;
  std::vector<std::string> OffsetParams;
  std::size_t MagicBudget = 0;
  std::size_t EmittedMagic = 0;
};

template <std::size_t N> using SArr = std::array<const char *, N>;

template <typename T, std::size_t N>
const T &pickArray(const std::array<T, N> &Items) {
  const auto Idx = *rc::gen::inRange<std::size_t>(0, N);
  return Items[Idx];
}

const std::string &pickVec(const std::vector<std::string> &Items) {
  const auto Idx = *rc::gen::inRange<std::size_t>(0, Items.size());
  return Items[Idx];
}

std::size_t countDiagCode(const std::vector<Diagnostic> &Diags,
                          std::string_view Code) {
  std::size_t Count = 0;
  for (const auto &Diag : Diags) {
    if (Diag.Code == Code) {
      ++Count;
    }
  }
  return Count;
}

bool hasDiagCode(const std::vector<Diagnostic> &Diags, std::string_view Code) {
  return countDiagCode(Diags, Code) > 0;
}

std::vector<TextEdit> collectL001Edits(const std::vector<Diagnostic> &Diags) {
  std::vector<TextEdit> Edits;
  for (const auto &Diag : Diags) {
    if (Diag.Code != "L001") {
      continue;
    }
    for (const auto &Fix : Diag.Fixes) {
      Edits.insert(Edits.end(), Fix.Edits.begin(), Fix.Edits.end());
    }
  }
  return Edits;
}

std::string genIntMagicText(std::size_t MinVal, std::size_t MaxVal) {
  for (std::size_t Try = 0; Try < 8; ++Try) {
    const auto Val = *rc::gen::inRange<std::size_t>(MinVal, MaxVal + 1);
    if (Val != 0 && Val != 1 && Val != 100) {
      return std::to_string(Val);
    }
  }
  return "7";
}

std::string genNonMagicNumText(bool IntegerOnly) {
  if (IntegerOnly) {
    return *rc::gen::arbitrary<bool>() ? "1" : "100";
  }
  static constexpr SArr<5> kVals = {"0", "1", "100", "0.0", "1.0"};
  return pickArray(kVals);
}

std::string genNumberLit(GenContext &Ctx, bool IntegerOnly,
                         bool AllowUnaryMinus, bool ForceMagic) {
  const bool UseMagic =
      Ctx.MagicBudget > 0 && (ForceMagic || *rc::gen::arbitrary<bool>());
  if (!UseMagic) {
    return genNonMagicNumText(IntegerOnly);
  }
  --Ctx.MagicBudget;
  ++Ctx.EmittedMagic;
  if (IntegerOnly) {
    return genIntMagicText(2, 250);
  }
  if (AllowUnaryMinus && *rc::gen::arbitrary<bool>()) {
    static constexpr SArr<4> kNegMagic = {"-1", "-2.5", "-0.75", "-100"};
    return pickArray(kNegMagic);
  }
  static constexpr std::array<const char *, 6> kPosMagic = {
      "2", "15", "2.5", "0.05", "14.25", "271.35"};
  return pickArray(kPosMagic);
}

std::string genPeriodArg(GenContext &Ctx) {
  if (!Ctx.PeriodParams.empty() && *rc::gen::arbitrary<bool>()) {
    return pickVec(Ctx.PeriodParams);
  }
  if (Ctx.MagicBudget > 0 && *rc::gen::arbitrary<bool>()) {
    --Ctx.MagicBudget;
    ++Ctx.EmittedMagic;
    return genIntMagicText(2, 50);
  }
  return *rc::gen::arbitrary<bool>() ? "1" : "100";
}

std::string genOffsetSuffix(GenContext &Ctx) {
  const auto Mode = *rc::gen::inRange<int>(0, 4);
  if (Mode == 0) {
    return "";
  }
  if (Mode == 1 && !Ctx.OffsetParams.empty()) {
    return "[" + pickVec(Ctx.OffsetParams) + "]";
  }
  if (Mode == 2 && Ctx.MagicBudget > 0) {
    --Ctx.MagicBudget;
    ++Ctx.EmittedMagic;
    return "[" + genIntMagicText(2, 50) + "]";
  }
  return *rc::gen::arbitrary<bool>() ? "[0]" : "[1]";
}

struct ExprPiece {
  std::string Text;
  int Prec = 0;
  bool IsPosUnit = false;
};

std::string maybeParens(const ExprPiece &Piece, bool Force) {
  if (Force) {
    return "(" + Piece.Text + ")";
  }
  return Piece.Text;
}

ExprPiece genNumberExpr(GenContext &Ctx, std::size_t Depth, bool InStatefulArg,
                        bool AllowUnaryMinus) {
  static constexpr SArr<5> kSeries = {"open", "high", "low", "close", "volume"};
  static constexpr std::array<const char *, 5> kPortfolio = {
      "position", "entry_price", "cash", "equity", "bars_in_position"};
  static constexpr std::array<const char *, 7> kSeriesFuncs = {
      "sma", "ema", "rsi", "stdev", "highest", "lowest", "change"};

  const int MaxKind = (Depth == 0) ? 4 : 8;
  const int Kind = *rc::gen::inRange<int>(0, MaxKind);
  if (Kind == 0) {
    return {std::string(pickArray(kSeries)) + genOffsetSuffix(Ctx), 0};
  }
  if (Kind == 1 && !InStatefulArg) {
    return {std::string(pickArray(kPortfolio)), 0};
  }
  if (Kind == 2 && !Ctx.Params.empty()) {
    const auto Idx = *rc::gen::inRange<std::size_t>(0, Ctx.Params.size());
    return {Ctx.Params[Idx].Name, 0};
  }
  if (Kind <= 3) {
    const std::string Lit = genNumberLit(Ctx, false, AllowUnaryMinus, false);
    const int Prec = (!Lit.empty() && Lit.front() == '-') ? 1 : 0;
    const bool IsUnit =
        (Lit == "1" || Lit == "1.0" || Lit == "100" || Lit == "100.0");
    return {Lit, Prec, IsUnit};
  }
  if (Kind == 4) {
    const int SubKind = *rc::gen::inRange<int>(0, 3);
    if (SubKind == 0) {
      return {"atr(" + genPeriodArg(Ctx) + ")" + genOffsetSuffix(Ctx), 0};
    }
    const std::string Fn = pickArray(kSeriesFuncs);
    const ExprPiece Arg = genNumberExpr(Ctx, Depth - 1, true, true);
    return {Fn + "(" + Arg.Text + ", " + genPeriodArg(Ctx) + ")" +
                genOffsetSuffix(Ctx),
            0};
  }
  if (Kind == 5) {
    const int FnKind = *rc::gen::inRange<int>(0, 4);
    const ExprPiece A = genNumberExpr(Ctx, Depth - 1, InStatefulArg, true);
    if (FnKind < 2) {
      const char *Fn = (FnKind == 0) ? "abs" : "sqrt";
      return {std::string(Fn) + "(" + A.Text + ")", 0};
    }
    const ExprPiece B = genNumberExpr(Ctx, Depth - 1, InStatefulArg, true);
    const char *Fn = (FnKind == 2) ? "min" : "max";
    return {std::string(Fn) + "(" + A.Text + ", " + B.Text + ")", 0};
  }
  if (Kind == 6) {
    const ExprPiece Inner = genNumberExpr(Ctx, Depth - 1, InStatefulArg, false);
    if (Inner.IsPosUnit) {
      if (Ctx.MagicBudget == 0) {
        return Inner;
      }
      --Ctx.MagicBudget;
      ++Ctx.EmittedMagic;
    }
    return {"-" + maybeParens(Inner, Inner.Prec >= 1), 1};
  }
  static constexpr std::array<const char *, 4> kOps = {"+", "-", "*", "/"};
  const int OpIdx = *rc::gen::inRange<int>(0, 4);
  const int OpPrec = (OpIdx >= 2) ? 2 : 3;
  const ExprPiece Lhs = genNumberExpr(Ctx, Depth - 1, InStatefulArg, true);
  const ExprPiece Rhs = genNumberExpr(Ctx, Depth - 1, InStatefulArg, true);
  const std::string Out = maybeParens(Lhs, Lhs.Prec > OpPrec) + " " +
                          kOps[static_cast<std::size_t>(OpIdx)] + " " +
                          maybeParens(Rhs, Rhs.Prec >= OpPrec);
  if (*rc::gen::inRange<int>(0, 5) == 0) {
    return {"(" + Out + ")", 0};
  }
  return {Out, OpPrec};
}

ExprPiece genBoolExpr(GenContext &Ctx, std::size_t Depth) {
  static constexpr SArr<6> kCmpOps = {"<", "<=", ">", ">=", "==", "!="};

  const int MaxKind = (Depth == 0) ? 4 : 8;
  const int Kind = *rc::gen::inRange<int>(0, MaxKind);
  if (Kind == 0) {
    return {*rc::gen::arbitrary<bool>() ? "true" : "false", 0};
  }
  if (Kind == 1) {
    const ExprPiece Arg = genNumberExpr(Ctx, Depth, false, true);
    return {"is_na(" + Arg.Text + ")", 0};
  }
  if (Kind == 2) {
    const char *Fn = *rc::gen::arbitrary<bool>() ? "crossover" : "crossunder";
    const ExprPiece A = genNumberExpr(Ctx, Depth, true, true);
    const ExprPiece B = genNumberExpr(Ctx, Depth, true, true);
    return {std::string(Fn) + "(" + A.Text + ", " + B.Text + ")", 0};
  }
  if (Kind <= 4) {
    const ExprPiece A = genNumberExpr(Ctx, Depth, false, true);
    const ExprPiece B = genNumberExpr(Ctx, Depth, false, true);
    return {A.Text + " " + pickArray(kCmpOps) + " " + B.Text, 1};
  }
  if (Kind == 5) {
    const ExprPiece Inner = genBoolExpr(Ctx, Depth - 1);
    return {"not " + maybeParens(Inner, Inner.Prec >= 2), 2};
  }
  if (Kind == 6) {
    const ExprPiece Lhs = genBoolExpr(Ctx, Depth - 1);
    const ExprPiece Rhs = genBoolExpr(Ctx, Depth - 1);
    const char *Op = *rc::gen::arbitrary<bool>() ? "==" : "!=";
    return {maybeParens(Lhs, Lhs.Prec >= 1) + " " + Op + " " +
                maybeParens(Rhs, Rhs.Prec >= 1),
            1};
  }
  const bool IsAnd = *rc::gen::arbitrary<bool>();
  const int OpPrec = IsAnd ? 3 : 4;
  const ExprPiece Lhs = genBoolExpr(Ctx, Depth - 1);
  const ExprPiece Rhs = genBoolExpr(Ctx, Depth - 1);
  const std::string Out = maybeParens(Lhs, Lhs.Prec > OpPrec) +
                          (IsAnd ? " and " : " or ") +
                          maybeParens(Rhs, Rhs.Prec >= OpPrec);
  if (*rc::gen::inRange<int>(0, 5) == 0) {
    return {"(" + Out + ")", 0};
  }
  return {Out, OpPrec};
}

std::string genActionText(GenContext &Ctx) {
  const int Mode = *rc::gen::inRange<int>(0, 5);
  if (Mode == 0) {
    return "cancel";
  }
  const bool IsBuy = (Mode <= 2);
  std::string Out = IsBuy ? "buy " : "sell ";
  if (IsBuy) {
    static constexpr std::array<const char *, 4> kBuyUnits = {
        "lots", "% equity", "% cash", "% position"};
    const ExprPiece Sz = genNumberExpr(Ctx, 1, false, false);
    Out += Sz.Text + " " + pickArray(kBuyUnits);
  } else {
    const int UnitKind = *rc::gen::inRange<int>(0, 3);
    if (UnitKind == 0) {
      Out += "all";
    } else {
      const ExprPiece Sz = genNumberExpr(Ctx, 1, false, false);
      Out += Sz.Text + (UnitKind == 1 ? " lots" : " % position");
    }
  }
  const int MaxOrder = IsBuy ? 4 : 5;
  const int OrderKind = *rc::gen::inRange<int>(0, MaxOrder);
  if (OrderKind == 1) {
    Out += " market";
  } else if (OrderKind == 2) {
    Out += " limit " + genNumberExpr(Ctx, 1, false, false).Text;
  } else if (OrderKind == 3) {
    Out += " stop " + genNumberExpr(Ctx, 1, false, false).Text;
  } else if (OrderKind == 4) {
    Out += " take_profit " + genNumberExpr(Ctx, 1, false, false).Text;
  }
  const int TifKind = *rc::gen::inRange<int>(0, 3);
  if (TifKind == 1) {
    Out += " day";
  } else if (TifKind == 2) {
    Out += " gtc";
  }
  return Out;
}
std::string genBecauseText(GenContext &Ctx) {
  static constexpr std::array<const char *, 6> kLiterals = {
      "a ", "{{b}} ", "\\\"c\\\" ", "\\\\ ", "вход ", "{{1}}"};
  const int Parts = *rc::gen::inRange<int>(1, 4);
  std::string Out = "\"";
  bool HasContent = false;
  for (int I = 0; I < Parts; ++I) {
    if (*rc::gen::arbitrary<bool>()) {
      Out += pickArray(kLiterals);
      HasContent = true;
    } else {
      Out.push_back('{');
      if (*rc::gen::arbitrary<bool>()) {
        Out += genNumberExpr(Ctx, 1, false, true).Text;
      } else {
        Out += genBoolExpr(Ctx, 1).Text;
      }
      Out.push_back('}');
      HasContent = true;
    }
  }
  if (!HasContent) {
    Out += "ok";
  }
  Out.push_back('"');
  return Out;
}

struct GeneratedProgram {
  std::string Source;
  std::size_t EmittedMagic = 0;
};

rc::Gen<GeneratedProgram> genValidProgram(GenConfig Cfg) {
  return rc::gen::exec([Cfg] {
    GenContext Ctx;
    Ctx.MagicBudget = Cfg.AllowMagic ? Cfg.MaxMagic : 0;

    std::string Out;
    if (Cfg.AllowComments && *rc::gen::arbitrary<bool>()) {
      Out += "# generated strategy\n";
      if (*rc::gen::arbitrary<bool>()) {
        Out += "\n";
      }
    }

    const auto ParamCount =
        *rc::gen::inRange<std::size_t>(Cfg.MinParams, Cfg.MaxParams + 1);
    for (std::size_t I = 0; I < ParamCount; ++I) {
      ParamSpec Spec;
      Spec.Name = "p_" + std::to_string(I);
      const int Kind = *rc::gen::inRange<int>(0, 3);
      Spec.IsInt = (Kind != 0);
      Spec.CanUseAsPeriod = (Kind == 1);
      Spec.CanUseAsOffset = (Kind >= 1) && Cfg.AllowNonNegParamOffsets;

      Out += "param " + Spec.Name + " = ";
      if (Kind == 0) {
        static constexpr std::array<const char *, 4> kFloats = {
            "-2.5", "0.5", "12.25", "25.0"};
        Out += pickArray(kFloats);
        if (*rc::gen::arbitrary<bool>()) {
          Out += " range -10.0..50.0 step 0.5";
        }
      } else if (Kind == 1) {
        const auto Def = *rc::gen::inRange<int>(1, 51);
        Out += std::to_string(Def);
        if (*rc::gen::arbitrary<bool>()) {
          Out += " range 1..100 step 1";
        }
        Ctx.PeriodParams.push_back(Spec.Name);
        if (Spec.CanUseAsOffset) {
          Ctx.OffsetParams.push_back(Spec.Name);
        }
      } else {
        const auto Def = *rc::gen::inRange<int>(0, 51);
        Out += std::to_string(Def);
        if (*rc::gen::arbitrary<bool>()) {
          Out += " range 0..100 step 1";
        }
        if (Spec.CanUseAsOffset) {
          Ctx.OffsetParams.push_back(Spec.Name);
        }
      }
      if (Cfg.AllowComments && *rc::gen::inRange<int>(0, 4) == 0) {
        Out += " # param comment";
      }
      Out.push_back('\n');
      Ctx.Params.push_back(std::move(Spec));
    }

    if (ParamCount > 0) {
      Out.push_back('\n');
    }

    const auto RuleCount =
        *rc::gen::inRange<std::size_t>(Cfg.MinRules, Cfg.MaxRules + 1);
    for (std::size_t I = 0; I < RuleCount; ++I) {
      static constexpr std::array<const char *, 4> kNameSuffix = {
          "", " {tag}", " \\\"q\\\"", " \\\\s"};
      const std::string RuleName =
          "r_" + std::to_string(I) + pickArray(kNameSuffix);
      std::string Cond = genBoolExpr(Ctx, kMaxGenDepth).Text;
      if (I + 1 == RuleCount && Cfg.RequireMagic && Ctx.EmittedMagic == 0) {
        if (Ctx.MagicBudget >= 2) {
          Cond = "(" + Cond + ") and change(close, 5) < -2.5";
          Ctx.MagicBudget -= 2;
          Ctx.EmittedMagic += 2;
        } else if (Ctx.MagicBudget >= 1) {
          Cond = "(" + Cond + ") and close > 2.5";
          --Ctx.MagicBudget;
          ++Ctx.EmittedMagic;
        }
      }
      const std::string Act = genActionText(Ctx);
      const std::string Bec = genBecauseText(Ctx);
      Out += "rule \"" + RuleName + "\"\n";
      Out += "  when " + Cond + "\n";
      Out += "  then " + Act + "\n";
      Out += "  because " + Bec + "\n\n";
    }

    return GeneratedProgram{std::move(Out), Ctx.EmittedMagic};
  });
}
RC_GTEST_PROP(DslProperty, ParsePrintRoundTrip, ()) {
  const auto Generated = *genValidProgram(GenConfig{});
  const auto First = parse(Generated.Source);
  RC_ASSERT(!First.HasSyntaxErrors);

  const std::string Printed = print(First.Ast);
  const auto Second = parse(Printed);
  RC_ASSERT(!Second.HasSyntaxErrors);
  RC_ASSERT(astEqual(First.Ast, Second.Ast));
  RC_ASSERT(print(Second.Ast) == Printed);
}

RC_GTEST_PROP(DslProperty, LinterFixesAreValidAndIdempotent, ()) {
  const auto InitParams = *rc::gen::inRange<std::size_t>(0, 49);
  GenConfig Cfg;
  Cfg.MinParams = InitParams;
  Cfg.MaxParams = InitParams;
  Cfg.MinRules = 1;
  Cfg.MaxRules = 4;
  Cfg.MaxMagic = std::min<std::size_t>(12, kMaxParams - InitParams);
  Cfg.AllowMagic = true;
  Cfg.RequireMagic = true;

  const auto Generated = *genValidProgram(Cfg);
  const auto Res = check(Generated.Source);
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-SYN"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-TYPE"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-BEC"));

  const auto Edits = collectL001Edits(Res.Diagnostics);
  RC_ASSERT(!Edits.empty());

  const std::string Fixed = applyEdits(Generated.Source, Edits);
  const auto After = check(Fixed);
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-SYN"));
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-TYPE"));
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-BEC"));
  RC_ASSERT(countDiagCode(After.Diagnostics, "L001") == 0U);
}

RC_GTEST_PROP(DslProperty, LinterRespectsMaxParamsBoundary, ()) {
  const auto InitParams = *rc::gen::inRange<std::size_t>(45, 51);
  const auto TargetMagic = *rc::gen::inRange<std::size_t>(1, 11);
  GenConfig Cfg;
  Cfg.MinParams = InitParams;
  Cfg.MaxParams = InitParams;
  Cfg.MinRules = 1;
  Cfg.MaxRules = 3;
  Cfg.MaxMagic = TargetMagic;
  Cfg.AllowMagic = true;
  Cfg.RequireMagic = true;

  const auto Generated = *genValidProgram(Cfg);
  const auto Res = check(Generated.Source);
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-SYN"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-TYPE"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-BEC"));

  const std::size_t MagicCount = countDiagCode(Res.Diagnostics, "L001");
  RC_ASSERT(MagicCount >= 1U);

  const auto Edits = collectL001Edits(Res.Diagnostics);
  const std::string Fixed = applyEdits(Generated.Source, Edits);
  const auto After = check(Fixed);
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-SYN"));
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-TYPE"));
  RC_ASSERT(!hasDiagCode(After.Diagnostics, "E-BEC"));
  RC_ASSERT(After.Params.size() <= kMaxParams);

  for (const auto &Diag : After.Diagnostics) {
    if (Diag.Code == "L001") {
      RC_ASSERT(Diag.Fixes.empty());
    }
  }
  if (InitParams + MagicCount <= kMaxParams) {
    RC_ASSERT(countDiagCode(After.Diagnostics, "L001") == 0U);
  }
}

RC_GTEST_PROP(DslProperty, NoLookaheadErrorOnFutureFreePrograms, ()) {
  const auto Generated = *genValidProgram(GenConfig{});
  const auto Res = check(Generated.Source);
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-SYN"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-TYPE"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-LA01"));
  RC_ASSERT(!hasDiagCode(Res.Diagnostics, "E-LA02"));
  RC_ASSERT(Res.Lookahead != LookaheadLevel::Error);
}

RC_GTEST_PROP(DslProperty, NegativeOffsetAlwaysFlagged, ()) {
  GenConfig Cfg;
  Cfg.MaxParams = 8;
  const auto Generated = *genValidProgram(Cfg);
  const int Mode = *rc::gen::inRange<int>(0, 3);
  const auto K = *rc::gen::inRange<int>(1, 100);

  const std::string KStr = std::to_string(K);
  std::string Src;
  if (Mode == 0) {
    Src = Generated.Source;
    Src += "rule \"la_probe\" when close[-" + KStr;
    Src += "] > 0 then cancel because \"probe\"\n";
  } else if (Mode == 1) {
    Src = "param bad_off = 0 range -" + KStr + "..5 step 1\n";
    Src += Generated.Source;
    Src += "rule \"la_probe\" when close[bad_off] > 0 ";
    Src += "then cancel because \"probe\"\n";
  } else {
    Src = "param bad_off = -" + KStr + "\n";
    Src += Generated.Source;
    Src += "rule \"la_probe\" when close[bad_off] > 0 ";
    Src += "then cancel because \"probe\"\n";
  }

  const auto Res = check(Src);
  RC_ASSERT(hasDiagCode(Res.Diagnostics, "E-LA01") ||
            hasDiagCode(Res.Diagnostics, "E-LA02"));
  RC_ASSERT(Res.Lookahead == LookaheadLevel::Error);
}

RC_GTEST_PROP(DslProperty, CheckIsDeterministic, ()) {
  const auto Generated = *genValidProgram(GenConfig{});
  std::string Src = Generated.Source;
  const int MutKind = *rc::gen::inRange<int>(0, 4);
  if (MutKind == 1 && Src.size() > 8U) {
    const auto Cut = *rc::gen::inRange<std::size_t>(1, Src.size());
    Src.resize(Cut);
  } else if (MutKind == 2 && !Src.empty()) {
    static constexpr std::array<const char *, 4> kTokens = {
        " ??? ", " close[-3] ", " and ", " \"unterminated "};
    const auto Pos = *rc::gen::inRange<std::size_t>(0, Src.size());
    Src.insert(Pos, pickArray(kTokens));
  } else if (MutKind == 3) {
    Src += "\nrule \"dup\" when bad_id > 0 then buy all\n";
  }

  const auto Run1 = check(Src);
  const auto Run2 = check(Src);
  RC_ASSERT(Run1.Diagnostics == Run2.Diagnostics);
  RC_ASSERT(Run1.Lookahead == Run2.Lookahead);
  RC_ASSERT(Run1.Params == Run2.Params);
}

} // namespace
} // namespace dsl
