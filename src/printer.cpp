#include "dsl/check.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace dsl {
namespace {

enum class Precedence {
  Primary = 1,
  UnaryNeg = 2,
  Mul = 3,
  Add = 4,
  Compare = 5,
  Not = 6,
  And = 7,
  Or = 8,
};

Precedence exprPrecedence(const Expr &Node) noexcept {
  return std::visit(
      [](const auto &Inner) -> Precedence {
        using T = std::decay_t<decltype(Inner)>;
        if constexpr (std::is_same_v<T, NumberLitExpr> ||
                      std::is_same_v<T, BoolLitExpr> ||
                      std::is_same_v<T, IdentExpr> ||
                      std::is_same_v<T, OffsetExpr> ||
                      std::is_same_v<T, CallExpr>) {
          return Precedence::Primary;
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
          return Inner.Op == UnaryOp::Neg ? Precedence::UnaryNeg
                                          : Precedence::Not;
        } else {
          switch (Inner.Op) {
          case BinaryOp::Mul:
          case BinaryOp::Div:
            return Precedence::Mul;
          case BinaryOp::Add:
          case BinaryOp::Sub:
            return Precedence::Add;
          case BinaryOp::Eq:
          case BinaryOp::Ne:
          case BinaryOp::Lt:
          case BinaryOp::Le:
          case BinaryOp::Gt:
          case BinaryOp::Ge:
            return Precedence::Compare;
          case BinaryOp::And:
            return Precedence::And;
          case BinaryOp::Or:
            return Precedence::Or;
          }
          return Precedence::Or;
        }
      },
      Node.Node);
}

std::string_view binaryOpText(BinaryOp Op) noexcept {
  switch (Op) {
  case BinaryOp::Or:
    return "or";
  case BinaryOp::And:
    return "and";
  case BinaryOp::Eq:
    return "==";
  case BinaryOp::Ne:
    return "!=";
  case BinaryOp::Lt:
    return "<";
  case BinaryOp::Le:
    return "<=";
  case BinaryOp::Gt:
    return ">";
  case BinaryOp::Ge:
    return ">=";
  case BinaryOp::Add:
    return "+";
  case BinaryOp::Sub:
    return "-";
  case BinaryOp::Mul:
    return "*";
  case BinaryOp::Div:
    return "/";
  }
  return "";
}

std::string formatDecimalDouble(double Value, bool ForceFloat) {
  if (!std::isfinite(Value)) {
    return "0";
  }
  std::array<char, 512> Buf{};
  const auto Res = std::to_chars(Buf.data(), Buf.data() + Buf.size(), Value,
                                 std::chars_format::fixed);
  std::string Out;
  if (Res.ec == std::errc{}) {
    Out.assign(Buf.data(), Res.ptr);
  } else {
    Out = std::to_string(Value);
  }
  const std::size_t DotPos = Out.find('.');
  if (DotPos != std::string::npos) {
    while (Out.size() > DotPos + 2 && Out.back() == '0') {
      Out.pop_back();
    }
    if (!ForceFloat && Out.size() == DotPos + 2 && Out.back() == '0') {
      Out.pop_back();
      Out.pop_back();
    }
  } else if (ForceFloat) {
    Out += ".0";
  }
  if (Out == "-0") {
    Out = "0";
  } else if (Out == "-0.0") {
    Out = "0.0";
  }
  return Out;
}

std::string formatParamNumber(const std::string &RawText, double Value,
                              bool IsInteger) {
  if (!RawText.empty()) {
    if (Value < 0.0 && RawText.front() != '-') {
      return "-" + RawText;
    }
    return RawText;
  }
  return formatDecimalDouble(Value, !IsInteger);
}

void appendEscapedString(std::string &Out, std::string_view Text,
                         bool EscapeBraces) {
  for (char Ch : Text) {
    if (Ch == '\\') {
      Out += "\\\\";
    } else if (Ch == '"') {
      Out += "\\\"";
    } else if (EscapeBraces && Ch == '{') {
      Out += "{{";
    } else if (EscapeBraces && Ch == '}') {
      Out += "}}";
    } else {
      Out.push_back(Ch);
    }
  }
}

void printExprTo(std::string &Out, const Expr &Node);

void printChildWithParens(std::string &Out, const Expr &Child,
                          bool NeedParens) {
  if (NeedParens) {
    Out.push_back('(');
  }
  printExprTo(Out, Child);
  if (NeedParens) {
    Out.push_back(')');
  }
}

void printExprTo(std::string &Out, const Expr &Node) {
  std::visit(
      [&](const auto &Inner) {
        using T = std::decay_t<decltype(Inner)>;
        if constexpr (std::is_same_v<T, NumberLitExpr>) {
          if (!Inner.RawText.empty()) {
            Out += Inner.RawText;
          } else {
            Out += formatDecimalDouble(Inner.Value, !Inner.IsInteger);
          }
        } else if constexpr (std::is_same_v<T, BoolLitExpr>) {
          Out += Inner.Value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, IdentExpr>) {
          Out += Inner.Name;
        } else if constexpr (std::is_same_v<T, OffsetExpr>) {
          if (Inner.Target) {
            printExprTo(Out, *Inner.Target);
          }
          Out.push_back('[');
          if (Inner.HasUnaryMinus) {
            Out.push_back('-');
          }
          if (Inner.OffsetIsIdent) {
            Out += Inner.OffsetIdent;
          } else {
            Out += std::to_string(Inner.OffsetInt);
          }
          Out.push_back(']');
        } else if constexpr (std::is_same_v<T, CallExpr>) {
          Out += Inner.Callee;
          Out.push_back('(');
          for (std::size_t I = 0; I < Inner.Args.size(); ++I) {
            if (I > 0) {
              Out += ", ";
            }
            if (Inner.Args[I]) {
              printExprTo(Out, *Inner.Args[I]);
            }
          }
          Out.push_back(')');
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
          const Precedence ChildPrec =
              Inner.Operand ? exprPrecedence(*Inner.Operand)
                            : Precedence::Primary;
          if (Inner.Op == UnaryOp::Neg) {
            Out.push_back('-');
            const bool NeedParens =
                static_cast<int>(ChildPrec) >=
                static_cast<int>(Precedence::UnaryNeg);
            if (Inner.Operand) {
              printChildWithParens(Out, *Inner.Operand, NeedParens);
            }
          } else {
            Out += "not ";
            const bool NeedParens =
                static_cast<int>(ChildPrec) >=
                static_cast<int>(Precedence::Not);
            if (Inner.Operand) {
              printChildWithParens(Out, *Inner.Operand, NeedParens);
            }
          }
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
          const Precedence ParentPrec = exprPrecedence(Node);
          const Precedence LeftPrec =
              Inner.Left ? exprPrecedence(*Inner.Left) : Precedence::Primary;
          const Precedence RightPrec =
              Inner.Right ? exprPrecedence(*Inner.Right) : Precedence::Primary;
          const bool IsComparison = (ParentPrec == Precedence::Compare);
          const bool LeftParens =
              IsComparison ? (static_cast<int>(LeftPrec) >=
                              static_cast<int>(ParentPrec))
                           : (static_cast<int>(LeftPrec) >
                              static_cast<int>(ParentPrec));
          const bool RightParens =
              static_cast<int>(RightPrec) >= static_cast<int>(ParentPrec);
          if (Inner.Left) {
            printChildWithParens(Out, *Inner.Left, LeftParens);
          }
          Out.push_back(' ');
          Out += binaryOpText(Inner.Op);
          Out.push_back(' ');
          if (Inner.Right) {
            printChildWithParens(Out, *Inner.Right, RightParens);
          }
        }
      },
      Node.Node);
}

void printActionTo(std::string &Out, const ActionNode &Act) {
  if (Act.Kind == ActionKind::Cancel) {
    Out += "cancel";
    return;
  }
  Out += (Act.Kind == ActionKind::Buy) ? "buy " : "sell ";
  switch (Act.Unit) {
  case SizeUnit::All:
    Out += "all";
    break;
  case SizeUnit::Lots:
    if (Act.SizeExpr) {
      printExprTo(Out, *Act.SizeExpr);
    }
    Out += " lots";
    break;
  case SizeUnit::PercentEquity:
    if (Act.SizeExpr) {
      printExprTo(Out, *Act.SizeExpr);
    }
    Out += " % equity";
    break;
  case SizeUnit::PercentCash:
    if (Act.SizeExpr) {
      printExprTo(Out, *Act.SizeExpr);
    }
    Out += " % cash";
    break;
  case SizeUnit::PercentPosition:
    if (Act.SizeExpr) {
      printExprTo(Out, *Act.SizeExpr);
    }
    Out += " % position";
    break;
  }

  switch (Act.OrderKind) {
  case OrderType::Market:
    break;
  case OrderType::Limit:
    Out += " limit ";
    if (Act.PriceExpr) {
      printExprTo(Out, *Act.PriceExpr);
    }
    break;
  case OrderType::Stop:
    Out += " stop ";
    if (Act.PriceExpr) {
      printExprTo(Out, *Act.PriceExpr);
    }
    break;
  case OrderType::TakeProfit:
    Out += " take_profit ";
    if (Act.PriceExpr) {
      printExprTo(Out, *Act.PriceExpr);
    }
    break;
  }

  if (Act.Tif == TimeInForce::Day) {
    Out += " day";
  }
}

std::vector<std::size_t> buildLineStarts(std::string_view Source) {
  std::vector<std::size_t> LineStarts;
  LineStarts.push_back(0);
  for (std::size_t I = 0; I < Source.size(); ++I) {
    if (Source[I] == '\r') {
      if (I + 1 < Source.size() && Source[I + 1] == '\n') {
        ++I;
      }
      LineStarts.push_back(I + 1);
    } else if (Source[I] == '\n') {
      LineStarts.push_back(I + 1);
    }
  }
  return LineStarts;
}

std::size_t utf8Step(std::string_view Text, std::size_t Pos) noexcept {
  if (Pos >= Text.size()) {
    return 0;
  }
  const auto Lead = static_cast<std::uint8_t>(Text[Pos]);
  std::size_t Len = 1;
  if (Lead < 0x80U) {
    Len = 1;
  } else if ((Lead & 0xE0U) == 0xC0U) {
    Len = 2;
  } else if ((Lead & 0xF0U) == 0xE0U) {
    Len = 3;
  } else if ((Lead & 0xF8U) == 0xF0U) {
    Len = 4;
  }
  return (Pos + Len <= Text.size()) ? Len : (Text.size() - Pos);
}

std::size_t posToByteOffset(std::string_view Source,
                            const std::vector<std::size_t> &LineStarts,
                            SourcePos Pos) noexcept {
  if (Pos.Line == 0 || LineStarts.empty()) {
    return 0;
  }
  if (Pos.Line > LineStarts.size()) {
    return Source.size();
  }
  std::size_t ByteOffset = LineStarts[Pos.Line - 1];
  const std::size_t TargetCol = Pos.Column > 0 ? Pos.Column : 1;
  std::size_t CurCol = 1;
  while (CurCol < TargetCol && ByteOffset < Source.size() &&
         Source[ByteOffset] != '\n' && Source[ByteOffset] != '\r') {
    ByteOffset += utf8Step(Source, ByteOffset);
    ++CurCol;
  }
  return ByteOffset;
}

struct ResolvedEdit {
  std::size_t StartByte = 0;
  std::size_t EndByte = 0;
  std::size_t OriginalIndex = 0;
  std::string_view Replacement;
};

} // namespace

std::string print(const Program &Ast) {
  std::string Out;
  for (const auto &Param : Ast.Params) {
    Out += "param ";
    Out += Param.Name;
    Out += " = ";
    Out += formatParamNumber(Param.DefaultRawText, Param.DefaultValue,
                             Param.DefaultIsInteger);
    if (Param.HasRange) {
      Out += " range ";
      Out += formatParamNumber(Param.RangeMinText, Param.RangeMin,
                               Param.RangeMinIsInt);
      Out += "..";
      Out += formatParamNumber(Param.RangeMaxText, Param.RangeMax,
                               Param.RangeMaxIsInt);
    }
    if (Param.HasStep) {
      Out += " step ";
      Out += formatParamNumber(Param.StepRawText, Param.StepValue,
                               Param.StepIsInteger);
    }
    Out.push_back('\n');
  }

  if (!Ast.Params.empty() && !Ast.Rules.empty()) {
    Out.push_back('\n');
  }

  for (std::size_t I = 0; I < Ast.Rules.size(); ++I) {
    if (I > 0) {
      Out.push_back('\n');
    }
    const auto &Rule = Ast.Rules[I];
    Out += "rule \"";
    appendEscapedString(Out, Rule.Name, false);
    Out += "\"\n";
    Out += "  when ";
    if (Rule.Condition) {
      printExprTo(Out, *Rule.Condition);
    }
    Out.push_back('\n');
    Out += "  then ";
    printActionTo(Out, Rule.Action);
    Out.push_back('\n');
    if (Rule.Because.Present) {
      Out += "  because \"";
      for (const auto &Part : Rule.Because.Parts) {
        if (!Part.IsExpr) {
          appendEscapedString(Out, Part.LiteralText, true);
        } else {
          Out.push_back('{');
          if (Part.Expression) {
            printExprTo(Out, *Part.Expression);
          }
          Out.push_back('}');
        }
      }
      Out += "\"\n";
    }
  }
  return Out;
}

std::string applyEdits(std::string_view Source,
                       const std::vector<TextEdit> &Edits) {
  const std::vector<std::size_t> LineStarts = buildLineStarts(Source);
  std::vector<ResolvedEdit> Sorted;
  Sorted.reserve(Edits.size());
  for (std::size_t I = 0; I < Edits.size(); ++I) {
    const std::size_t StartByte =
        posToByteOffset(Source, LineStarts, Edits[I].Range.Start);
    const std::size_t EndByte =
        posToByteOffset(Source, LineStarts, Edits[I].Range.End);
    const std::size_t ClampedEnd = std::max(StartByte, EndByte);
    Sorted.push_back(
        ResolvedEdit{StartByte, ClampedEnd, I, Edits[I].Replacement});
  }

  std::sort(Sorted.begin(), Sorted.end(),
            [](const ResolvedEdit &A, const ResolvedEdit &B) {
              if (A.StartByte != B.StartByte) {
                return A.StartByte > B.StartByte;
              }
              if (A.EndByte != B.EndByte) {
                return A.EndByte > B.EndByte;
              }
              return A.OriginalIndex > B.OriginalIndex;
            });

  std::string Result(Source);
  for (const auto &Edit : Sorted) {
    Result.replace(Edit.StartByte, Edit.EndByte - Edit.StartByte,
                   Edit.Replacement);
  }
  return Result;
}

} // namespace dsl
