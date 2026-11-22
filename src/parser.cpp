#include "dsl/check.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"
#include "internal/lexer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dsl {
namespace {

using internal::LexResult;
using internal::Token;
using internal::TokenKind;

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

std::string unescapeRuleName(std::string_view RawBody) {
  std::string Out;
  Out.reserve(RawBody.size());
  for (std::size_t I = 0; I < RawBody.size(); ++I) {
    if (RawBody[I] == '\\' && I + 1 < RawBody.size()) {
      const char NextCh = RawBody[I + 1];
      if (NextCh == '"' || NextCh == '\\') {
        Out.push_back(NextCh);
        ++I;
        continue;
      }
    }
    Out.push_back(RawBody[I]);
  }
  return Out;
}

bool isInlineBlank(std::string_view Text) noexcept {
  std::size_t I = 0;
  while (I < Text.size()) {
    const char Ch = Text[I];
    if (Ch == ' ' || Ch == '\t') {
      ++I;
      continue;
    }
    if (I + 1 < Text.size() && static_cast<unsigned char>(Ch) == 0xC2U &&
        static_cast<unsigned char>(Text[I + 1]) == 0xA0U) {
      I += 2;
      continue;
    }
    return false;
  }
  return true;
}

std::size_t computeExprHeight(const Expr &Node) noexcept {
  return std::visit(
      [](const auto &Inner) -> std::size_t {
        using T = std::decay_t<decltype(Inner)>;
        if constexpr (std::is_same_v<T, NumberLitExpr> ||
                      std::is_same_v<T, BoolLitExpr> ||
                      std::is_same_v<T, IdentExpr>) {
          return 1;
        } else if constexpr (std::is_same_v<T, OffsetExpr>) {
          return 1 + (Inner.Target ? computeExprHeight(*Inner.Target) : 0);
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
          return 1 + (Inner.Operand ? computeExprHeight(*Inner.Operand) : 0);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
          const std::size_t LeftH =
              Inner.Left ? computeExprHeight(*Inner.Left) : 0;
          const std::size_t RightH =
              Inner.Right ? computeExprHeight(*Inner.Right) : 0;
          return 1 + std::max(LeftH, RightH);
        } else {
          std::size_t MaxArgH = 0;
          for (const auto &Arg : Inner.Args) {
            if (Arg) {
              MaxArgH = std::max(MaxArgH, computeExprHeight(*Arg));
            }
          }
          return 1 + MaxArgH;
        }
      },
      Node.Node);
}

bool isComparisonToken(TokenKind Kind) noexcept {
  return Kind == TokenKind::Lt || Kind == TokenKind::LtEq ||
         Kind == TokenKind::Gt || Kind == TokenKind::GtEq ||
         Kind == TokenKind::EqEq || Kind == TokenKind::BangEq;
}

std::optional<BinaryOp> toBinaryCompareOp(TokenKind Kind) noexcept {
  switch (Kind) {
  case TokenKind::Lt:
    return BinaryOp::Lt;
  case TokenKind::LtEq:
    return BinaryOp::Le;
  case TokenKind::Gt:
    return BinaryOp::Gt;
  case TokenKind::GtEq:
    return BinaryOp::Ge;
  case TokenKind::EqEq:
    return BinaryOp::Eq;
  case TokenKind::BangEq:
    return BinaryOp::Ne;
  default:
    return std::nullopt;
  }
}

struct ParsedNum {
  double Value = 0.0;
  bool IsInteger = false;
  std::string RawText;
  SourceRange Range;
};

class Parser {
public:
  Parser(const std::vector<Token> &TokenList,
         std::vector<Diagnostic> InitialDiags, bool InitialSyntaxErrors)
      : Tokens(&TokenList), Diagnostics(std::move(InitialDiags)),
        HasSyntaxErrors(InitialSyntaxErrors) {
    for (const auto &Diag : Diagnostics) {
      if (Diag.Code == "E-SYN") {
        ++SyntaxErrorCount;
      }
    }
  }
  ParseResult parseProgram() {
    bool SeenRule = false;
    while (!atEnd() && !syntaxLimitReached()) {
      if (check(TokenKind::KwParam)) {
        if (SeenRule) {
          emitSyntaxError(peek().Range,
                          "Объявления param должны располагаться до rule");
          ParamStmt Ignored;
          if (!parseParam(Ignored)) {
            synchronizeToTopLevel();
          }
          continue;
        }
        ParamStmt Param;
        if (parseParam(Param)) {
          if (Ast.Params.size() >= kMaxParams) {
            emitSyntaxError(Param.Range,
                            "Превышено максимальное число параметров (50)");
          } else if (countNode(Param.Range)) {
            Ast.Params.push_back(std::move(Param));
          }
        } else {
          synchronizeToTopLevel();
        }
      } else if (check(TokenKind::KwRule)) {
        SeenRule = true;
        RuleStmt Rule;
        if (parseRule(Rule)) {
          if (Ast.Rules.size() >= kMaxRules) {
            emitSyntaxError(Rule.Range,
                            "Превышено максимальное число правил (100)");
          } else if (countNode(Rule.Range)) {
            Ast.Rules.push_back(std::move(Rule));
          }
        } else {
          synchronizeToTopLevel();
        }
      } else {
        emitSyntaxError(peek().Range, "Ожидалось объявление param или rule");
        advance();
        synchronizeToTopLevel();
      }
    }

    if (!SeenRule && !syntaxLimitReached()) {
      emitSyntaxError(peek().Range,
                      "Стратегия должна содержать хотя бы одно правило rule");
    }

    ParseResult Result;
    Result.Ast = std::move(Ast);
    Result.Diagnostics = std::move(Diagnostics);
    Result.HasSyntaxErrors = HasSyntaxErrors;
    return Result;
  }

  ExprPtr parseInlineOnly(std::size_t BaseNodeCount) {
    Ast.AstNodeCount = BaseNodeCount;
    ExprPtr Node = parseExprWithHeightCheck();
    if (!Node) {
      return nullptr;
    }
    if (!atEnd() && !syntaxLimitReached()) {
      emitSyntaxError(peek().Range, "Лишние токены в подстановке because");
      return nullptr;
    }
    return Node;
  }

  std::vector<Diagnostic> takeDiagnostics() { return std::move(Diagnostics); }
  bool hasSyntaxErrors() const noexcept { return HasSyntaxErrors; }
  std::size_t nodeCount() const noexcept { return Ast.AstNodeCount; }

private:
  const Token &peek(std::size_t Offset = 0) const noexcept {
    const std::size_t Idx = Pos + Offset;
    if (Idx >= Tokens->size()) {
      return Tokens->back();
    }
    return (*Tokens)[Idx];
  }

  const Token &previous() const noexcept {
    return (Pos > 0) ? (*Tokens)[Pos - 1] : (*Tokens)[0];
  }

  bool atEnd() const noexcept { return peek().Kind == TokenKind::Eof; }
  bool check(TokenKind Kind) const noexcept { return peek().Kind == Kind; }

  bool match(TokenKind Kind) noexcept {
    if (!check(Kind)) {
      return false;
    }
    advance();
    return true;
  }

  const Token &advance() noexcept {
    if (!atEnd()) {
      ++Pos;
    }
    return previous();
  }

  bool syntaxLimitReached() const noexcept {
    return SyntaxErrorCount >= kMaxSyntaxErrors;
  }

  void emitSyntaxError(SourceRange Range, std::string Message) {
    HasSyntaxErrors = true;
    if (SyntaxErrorCount >= kMaxSyntaxErrors) {
      return;
    }
    ++SyntaxErrorCount;
    Diagnostics.push_back(Diagnostic{
        "E-SYN", DiagnosticSeverity::Error, Range, std::move(Message), {}});
  }

  void emitMissingBecause(SourceRange Range) {
    Diagnostics.push_back(Diagnostic{
        "E-BEC",
        DiagnosticSeverity::Error,
        Range,
        "Отсутствует обязательная секция because после действия правила",
        {}});
  }

  bool countNode(SourceRange Range) {
    ++Ast.AstNodeCount;
    if (Ast.AstNodeCount > kMaxAstNodes) {
      if (!AstNodeLimitReported) {
        AstNodeLimitReported = true;
        emitSyntaxError(Range, "Превышено максимальное число узлов AST (2000)");
      }
      return false;
    }
    return true;
  }

  void synchronizeToTopLevel() noexcept {
    while (!atEnd() && !check(TokenKind::KwParam) &&
           !check(TokenKind::KwRule)) {
      advance();
    }
  }

  std::optional<ParsedNum> parseNumberLiteral(bool AllowMinus) {
    bool HasMinus = false;
    SourcePos StartPos = peek().Range.Start;
    if (AllowMinus && match(TokenKind::Minus)) {
      HasMinus = true;
    }
    if (!check(TokenKind::IntLit) && !check(TokenKind::FloatLit)) {
      emitSyntaxError(peek().Range, "Ожидалось число");
      return std::nullopt;
    }
    const Token &NumTok = advance();
    ParsedNum Out;
    Out.IsInteger = (NumTok.Kind == TokenKind::IntLit);
    Out.Value = HasMinus ? -NumTok.NumericValue : NumTok.NumericValue;
    Out.RawText = HasMinus ? ("-" + NumTok.Text) : NumTok.Text;
    Out.Range = {StartPos, NumTok.Range.End};
    return Out;
  }

  bool parseParam(ParamStmt &Out) {
    const Token &KwTok = advance();
    if (!check(TokenKind::Ident)) {
      emitSyntaxError(peek().Range, "Ожидалось имя параметра после param");
      return false;
    }
    const Token &NameTok = advance();
    Out.Name = NameTok.Text;
    Out.NameRange = NameTok.Range;
    if (!match(TokenKind::Eq)) {
      emitSyntaxError(peek().Range, "Ожидался знак '=' после имени параметра");
      return false;
    }
    const auto DefaultNum = parseNumberLiteral(true);
    if (!DefaultNum) {
      return false;
    }
    Out.DefaultValue = DefaultNum->Value;
    Out.DefaultIsInteger = DefaultNum->IsInteger;
    Out.DefaultRawText = DefaultNum->RawText;
    Out.DefaultRange = DefaultNum->Range;
    SourcePos EndPos = DefaultNum->Range.End;
    if (match(TokenKind::KwRange)) {
      Out.HasRange = true;
      const auto MinNum = parseNumberLiteral(true);
      if (!MinNum) {
        return false;
      }
      if (!match(TokenKind::DotDot)) {
        emitSyntaxError(peek().Range, "Ожидалось '..' в диапазоне range");
        return false;
      }
      const auto MaxNum = parseNumberLiteral(true);
      if (!MaxNum) {
        return false;
      }
      if (!match(TokenKind::KwStep)) {
        emitSyntaxError(peek().Range,
                        "Ожидалось ключевое слово step после range");
        return false;
      }
      const auto StepNum = parseNumberLiteral(false);
      if (!StepNum) {
        return false;
      }
      Out.RangeMin = MinNum->Value;
      Out.RangeMinIsInt = MinNum->IsInteger;
      Out.RangeMinText = MinNum->RawText;
      Out.RangeMinRange = MinNum->Range;
      Out.RangeMax = MaxNum->Value;
      Out.RangeMaxIsInt = MaxNum->IsInteger;
      Out.RangeMaxText = MaxNum->RawText;
      Out.RangeMaxRange = MaxNum->Range;
      Out.HasStep = true;
      Out.StepValue = StepNum->Value;
      Out.StepIsInteger = StepNum->IsInteger;
      Out.StepRawText = StepNum->RawText;
      Out.StepRange = StepNum->Range;
      EndPos = StepNum->Range.End;
    }

    Out.Range = {KwTok.Range.Start, EndPos};
    return true;
  }

  bool parseRule(RuleStmt &Out) {
    const Token &RuleTok = advance();
    if (!check(TokenKind::StringLit)) {
      emitSyntaxError(peek().Range, "Ожидалось строковое имя правила");
      return false;
    }
    const Token &NameTok = advance();
    Out.Name = unescapeRuleName(NameTok.Text);
    Out.NameRange = NameTok.Range;

    if (!match(TokenKind::KwWhen)) {
      emitSyntaxError(peek().Range, "Ожидалось ключевое слово when");
      return false;
    }
    Out.Condition = parseExprWithHeightCheck();
    if (!Out.Condition) {
      return false;
    }
    if (!match(TokenKind::KwThen)) {
      emitSyntaxError(peek().Range, "Ожидалось ключевое слово then");
      return false;
    }
    if (!parseAction(Out.Action)) {
      return false;
    }

    SourcePos EndPos = Out.Action.Range.End;
    if (check(TokenKind::KwBecause)) {
      const Token &BecTok = advance();
      if (!check(TokenKind::StringLit)) {
        emitSyntaxError(peek().Range,
                        "Ожидался строковый литерал после because");
        return false;
      }
      const Token &StrTok = advance();
      Out.Because.Present = true;
      Out.Because.Range = {BecTok.Range.Start, StrTok.Range.End};
      EndPos = StrTok.Range.End;
      parseBecauseString(StrTok, Out.Because);
    } else if (atEnd() || check(TokenKind::KwRule) ||
               check(TokenKind::KwParam)) {
      Out.Because.Present = false;
      emitMissingBecause(Out.Action.Range);
    } else {
      emitSyntaxError(peek().Range,
                      "Ожидалось ключевое слово because после действия");
      return false;
    }

    Out.Range = {RuleTok.Range.Start, EndPos};
    return true;
  }

  bool parseAction(ActionNode &Out) {
    const Token &ActTok = advance();
    if (ActTok.Kind == TokenKind::KwCancel) {
      Out.Kind = ActionKind::Cancel;
      Out.Range = ActTok.Range;
      return true;
    }
    if (ActTok.Kind == TokenKind::KwBuy) {
      Out.Kind = ActionKind::Buy;
    } else if (ActTok.Kind == TokenKind::KwSell) {
      Out.Kind = ActionKind::Sell;
    } else {
      emitSyntaxError(ActTok.Range, "Ожидалось действие buy, sell или cancel");
      return false;
    }

    if (check(TokenKind::KwAll)) {
      const Token &AllTok = advance();
      Out.Unit = SizeUnit::All;
      Out.SizeRange = AllTok.Range;
    } else {
      Out.SizeExpr = parseExprWithHeightCheck();
      if (!Out.SizeExpr) {
        return false;
      }
      const SourcePos SizeStart = Out.SizeExpr->Range.Start;
      if (match(TokenKind::KwLots)) {
        Out.Unit = SizeUnit::Lots;
        Out.SizeRange = {SizeStart, previous().Range.End};
      } else if (match(TokenKind::Percent)) {
        if (!check(TokenKind::Ident)) {
          emitSyntaxError(peek().Range,
                          "Ожидалось equity, cash или position после '%'");
          return false;
        }
        const Token &BaseTok = advance();
        if (BaseTok.Text == "equity") {
          Out.Unit = SizeUnit::PercentEquity;
        } else if (BaseTok.Text == "cash") {
          Out.Unit = SizeUnit::PercentCash;
        } else if (BaseTok.Text == "position") {
          Out.Unit = SizeUnit::PercentPosition;
        } else {
          emitSyntaxError(BaseTok.Range,
                          "Ожидалось equity, cash или position после '%'");
          return false;
        }
        Out.SizeRange = {SizeStart, BaseTok.Range.End};
      } else {
        emitSyntaxError(peek().Range,
                        "Ожидалось lots или '%' после размера заявки");
        return false;
      }
    }

    SourcePos EndPos = Out.SizeRange.End;
    if (check(TokenKind::KwMarket)) {
      const Token &OrdTok = advance();
      Out.OrderKind = OrderType::Market;
      Out.ExplicitOrder = true;
      Out.OrderRange = OrdTok.Range;
      EndPos = OrdTok.Range.End;
    } else if (check(TokenKind::KwLimit) || check(TokenKind::KwStop) ||
               check(TokenKind::KwTakeProfit)) {
      const Token &OrdTok = advance();
      if (OrdTok.Kind == TokenKind::KwLimit) {
        Out.OrderKind = OrderType::Limit;
      } else if (OrdTok.Kind == TokenKind::KwStop) {
        Out.OrderKind = OrderType::Stop;
      } else {
        Out.OrderKind = OrderType::TakeProfit;
      }
      Out.ExplicitOrder = true;
      Out.PriceExpr = parseExprWithHeightCheck();
      if (!Out.PriceExpr) {
        return false;
      }
      Out.OrderRange = {OrdTok.Range.Start, Out.PriceExpr->Range.End};
      EndPos = Out.OrderRange.End;
    }

    if (match(TokenKind::KwDay)) {
      Out.Tif = TimeInForce::Day;
      Out.ExplicitTif = true;
      EndPos = previous().Range.End;
    } else if (match(TokenKind::KwGtc)) {
      Out.Tif = TimeInForce::Gtc;
      Out.ExplicitTif = true;
      EndPos = previous().Range.End;
    }

    Out.Range = {ActTok.Range.Start, EndPos};
    return true;
  }
  void parseBecauseString(const Token &StrTok, BecauseClause &Out) {
    const std::string_view Body = StrTok.Text;
    const std::size_t Line = StrTok.Range.Start.Line;
    std::size_t CurCol = StrTok.Range.Start.Column + 1;
    std::size_t PartStartCol = CurCol;
    std::string LiteralBuf;

    auto FlushLiteral = [&](std::size_t EndCol) {
      if (!LiteralBuf.empty()) {
        TemplatePart Part;
        Part.IsExpr = false;
        Part.LiteralText = std::move(LiteralBuf);
        Part.Range = {{Line, PartStartCol}, {Line, EndCol}};
        Out.Parts.push_back(std::move(Part));
        LiteralBuf.clear();
      }
    };

    std::size_t I = 0;
    while (I < Body.size() && !syntaxLimitReached()) {
      const char Ch = Body[I];
      if (Ch == '\\' && I + 1 < Body.size() &&
          (Body[I + 1] == '"' || Body[I + 1] == '\\')) {
        LiteralBuf.push_back(Body[I + 1]);
        I += 2;
        CurCol += 2;
        continue;
      }
      if (Ch == '{') {
        if (I + 1 < Body.size() && Body[I + 1] == '{') {
          LiteralBuf.push_back('{');
          I += 2;
          CurCol += 2;
          continue;
        }
        FlushLiteral(CurCol);
        const std::size_t OpenBraceCol = CurCol;
        ++I;
        ++CurCol;
        const std::size_t ExprOriginCol = CurCol;
        const std::size_t ExprByteStart = I;
        std::size_t ExprByteEnd = I;
        std::size_t BraceDepth = 1;
        bool NestedBrace = false;

        while (I < Body.size() && BraceDepth > 0) {
          if (Body[I] == '{') {
            if (!NestedBrace) {
              emitSyntaxError({{Line, CurCol}, {Line, CurCol + 1}},
                              "Вложенная '{' запрещена внутри because");
              NestedBrace = true;
            }
            ++BraceDepth;
            ++I;
            ++CurCol;
          } else if (Body[I] == '}') {
            --BraceDepth;
            if (BraceDepth == 0) {
              ExprByteEnd = I;
            }
            ++I;
            ++CurCol;
          } else {
            I += utf8Step(Body, I);
            ++CurCol;
          }
        }

        if (BraceDepth > 0) {
          if (!NestedBrace) {
            emitSyntaxError({{Line, OpenBraceCol}, {Line, OpenBraceCol + 1}},
                            "Незакрытая '{' в строке because");
          }
          PartStartCol = CurCol;
          break;
        }

        const std::size_t CloseEndCol = CurCol;
        PartStartCol = CurCol;
        if (NestedBrace) {
          continue;
        }

        const std::string_view ExprSlice =
            Body.substr(ExprByteStart, ExprByteEnd - ExprByteStart);
        if (isInlineBlank(ExprSlice)) {
          emitSyntaxError({{Line, OpenBraceCol}, {Line, CloseEndCol}},
                          "Пустая подстановка '{}' в строке because");
          continue;
        }

        LexResult InlineLex =
            internal::lexInlineExpr(ExprSlice, {Line, ExprOriginCol});
        for (auto &Diag : InlineLex.Diagnostics) {
          if (Diag.Code == "E-SYN") {
            emitSyntaxError(Diag.Range, std::move(Diag.Message));
          }
        }
        if (InlineLex.HasFatalError || !InlineLex.Diagnostics.empty()) {
          continue;
        }

        Parser SubParser(InlineLex.Tokens, {}, false);
        SubParser.AstNodeLimitReported = AstNodeLimitReported;
        ExprPtr SubExpr = SubParser.parseInlineOnly(Ast.AstNodeCount);
        Ast.AstNodeCount = SubParser.nodeCount();
        AstNodeLimitReported = SubParser.AstNodeLimitReported;
        for (auto &Diag : SubParser.takeDiagnostics()) {
          if (Diag.Code == "E-SYN") {
            emitSyntaxError(Diag.Range, std::move(Diag.Message));
          }
        }
        if (SubExpr) {
          TemplatePart Part;
          Part.IsExpr = true;
          Part.Expression = std::move(SubExpr);
          Part.Range = {{Line, OpenBraceCol}, {Line, CloseEndCol}};
          Out.Parts.push_back(std::move(Part));
        }
        continue;
      }
      if (Ch == '}') {
        if (I + 1 < Body.size() && Body[I + 1] == '}') {
          LiteralBuf.push_back('}');
          I += 2;
          CurCol += 2;
          continue;
        }
        emitSyntaxError({{Line, CurCol}, {Line, CurCol + 1}},
                        "Непарная '}' в строке because");
        ++I;
        ++CurCol;
        continue;
      }
      const std::size_t Step = utf8Step(Body, I);
      LiteralBuf.append(Body.substr(I, Step));
      I += Step;
      ++CurCol;
    }

    FlushLiteral(CurCol);
  }

  ExprPtr makeExpr(SourceRange Range, ExprNode Node) {
    if (!countNode(Range)) {
      return nullptr;
    }
    auto Ptr = std::make_unique<Expr>();
    Ptr->Range = Range;
    Ptr->Node = std::move(Node);
    return Ptr;
  }

  ExprPtr parseExprWithHeightCheck() {
    ExprPtr Root = parseOrExpr();
    if (!Root) {
      return nullptr;
    }
    if (computeExprHeight(*Root) > kMaxExprDepth) {
      emitSyntaxError(Root->Range,
                      "Превышена максимальная высота выражения (100)");
      return nullptr;
    }
    return Root;
  }
  ExprPtr parseOrExpr() {
    if (CallDepth >= kMaxExprDepth * 4) {
      emitSyntaxError(peek().Range,
                      "Превышена максимальная глубина вложенности выражения");
      return nullptr;
    }
    ++CallDepth;
    struct DepthGuard {
      std::size_t &Depth;
      ~DepthGuard() { --Depth; }
    } Guard{CallDepth};

    ExprPtr Left = parseAndExpr();
    if (!Left) {
      return nullptr;
    }
    while (match(TokenKind::KwOr)) {
      ExprPtr Right = parseAndExpr();
      if (!Right) {
        return nullptr;
      }
      const SourceRange Span{Left->Range.Start, Right->Range.End};
      Left = makeExpr(
          Span, BinaryExpr{BinaryOp::Or, std::move(Left), std::move(Right)});
      if (!Left) {
        return nullptr;
      }
    }
    return Left;
  }

  ExprPtr parseAndExpr() {
    ExprPtr Left = parseNotExpr();
    if (!Left) {
      return nullptr;
    }
    while (match(TokenKind::KwAnd)) {
      ExprPtr Right = parseNotExpr();
      if (!Right) {
        return nullptr;
      }
      const SourceRange Span{Left->Range.Start, Right->Range.End};
      Left = makeExpr(
          Span, BinaryExpr{BinaryOp::And, std::move(Left), std::move(Right)});
      if (!Left) {
        return nullptr;
      }
    }
    return Left;
  }

  ExprPtr parseNotExpr() {
    if (check(TokenKind::KwNot)) {
      const Token &NotTok = advance();
      if (check(TokenKind::KwNot)) {
        emitSyntaxError(peek().Range,
                        "Цепочка 'not not' запрещена; используйте скобки");
        return nullptr;
      }
      ExprPtr Operand = parseCmpExpr();
      if (!Operand) {
        return nullptr;
      }
      const SourceRange Span{NotTok.Range.Start, Operand->Range.End};
      return makeExpr(Span, UnaryExpr{UnaryOp::Not, std::move(Operand)});
    }
    return parseCmpExpr();
  }

  ExprPtr parseCmpExpr() {
    ExprPtr Left = parseAddExpr();
    if (!Left) {
      return nullptr;
    }
    if (const auto CmpOp = toBinaryCompareOp(peek().Kind)) {
      advance();
      ExprPtr Right = parseAddExpr();
      if (!Right) {
        return nullptr;
      }
      if (isComparisonToken(peek().Kind)) {
        emitSyntaxError(peek().Range,
                        "Цепочки сравнений запрещены; используйте скобки");
        return nullptr;
      }
      const SourceRange Span{Left->Range.Start, Right->Range.End};
      BinaryExpr CmpNode{*CmpOp, std::move(Left), std::move(Right)};
      return makeExpr(Span, std::move(CmpNode));
    }
    return Left;
  }

  ExprPtr parseAddExpr() {
    ExprPtr Left = parseMulExpr();
    if (!Left) {
      return nullptr;
    }
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
      const Token &OpTok = advance();
      const BinaryOp Op =
          (OpTok.Kind == TokenKind::Plus) ? BinaryOp::Add : BinaryOp::Sub;
      ExprPtr Right = parseMulExpr();
      if (!Right) {
        return nullptr;
      }
      const SourceRange Span{Left->Range.Start, Right->Range.End};
      Left = makeExpr(Span, BinaryExpr{Op, std::move(Left), std::move(Right)});
      if (!Left) {
        return nullptr;
      }
    }
    return Left;
  }

  ExprPtr parseMulExpr() {
    ExprPtr Left = parseUnaryExpr();
    if (!Left) {
      return nullptr;
    }
    while (check(TokenKind::Star) || check(TokenKind::Slash)) {
      const Token &OpTok = advance();
      const BinaryOp Op =
          (OpTok.Kind == TokenKind::Star) ? BinaryOp::Mul : BinaryOp::Div;
      ExprPtr Right = parseUnaryExpr();
      if (!Right) {
        return nullptr;
      }
      const SourceRange Span{Left->Range.Start, Right->Range.End};
      Left = makeExpr(Span, BinaryExpr{Op, std::move(Left), std::move(Right)});
      if (!Left) {
        return nullptr;
      }
    }
    return Left;
  }

  ExprPtr parseUnaryExpr() {
    if (check(TokenKind::Minus)) {
      const Token &MinusTok = advance();
      if (check(TokenKind::Minus)) {
        emitSyntaxError(peek().Range,
                        "Цепочка '--' запрещена; используйте скобки");
        return nullptr;
      }
      ExprPtr Operand = parsePostfixExpr();
      if (!Operand) {
        return nullptr;
      }
      const SourceRange Span{MinusTok.Range.Start, Operand->Range.End};
      return makeExpr(Span, UnaryExpr{UnaryOp::Neg, std::move(Operand)});
    }
    return parsePostfixExpr();
  }
  ExprPtr parsePostfixExpr() {
    if (check(TokenKind::Ident)) {
      const bool IsCall = (peek(1).Kind == TokenKind::LParen);
      ExprPtr Target = IsCall ? parseCallExpr() : parseIdentExpr();
      if (!Target) {
        return nullptr;
      }
      if (match(TokenKind::LBracket)) {
        OffsetExpr Off;
        Off.Target = std::move(Target);
        const SourcePos OffStart = peek().Range.Start;
        if (match(TokenKind::Minus)) {
          Off.HasUnaryMinus = true;
        }
        if (check(TokenKind::IntLit)) {
          const Token &NumTok = advance();
          Off.OffsetIsIdent = false;
          Off.OffsetInt = NumTok.IntValue;
          Off.OffsetRange = {OffStart, NumTok.Range.End};
        } else if (check(TokenKind::Ident)) {
          const Token &IdTok = advance();
          Off.OffsetIsIdent = true;
          Off.OffsetIdent = IdTok.Text;
          Off.OffsetRange = {OffStart, IdTok.Range.End};
        } else {
          emitSyntaxError(peek().Range,
                          "Ожидалось целое число или имя параметра в [...]");
          return nullptr;
        }
        if (!match(TokenKind::RBracket)) {
          emitSyntaxError(peek().Range, "Ожидалась закрывающая ']'");
          return nullptr;
        }
        if (check(TokenKind::LBracket)) {
          emitSyntaxError(peek().Range,
                          "Повторная индексация '[...]' запрещена");
          return nullptr;
        }
        const SourceRange Span{Off.Target->Range.Start, previous().Range.End};
        return makeExpr(Span, std::move(Off));
      }
      return Target;
    }

    ExprPtr Primary;
    if (check(TokenKind::IntLit) || check(TokenKind::FloatLit)) {
      const Token &Tok = advance();
      NumberLitExpr Lit;
      Lit.Value = Tok.NumericValue;
      Lit.IsInteger = (Tok.Kind == TokenKind::IntLit);
      Lit.RawText = Tok.Text;
      Primary = makeExpr(Tok.Range, std::move(Lit));
    } else if (check(TokenKind::KwTrue) || check(TokenKind::KwFalse)) {
      const Token &Tok = advance();
      Primary = makeExpr(Tok.Range, BoolLitExpr{Tok.Kind == TokenKind::KwTrue});
    } else if (check(TokenKind::LParen)) {
      const Token &OpenTok = advance();
      ExprPtr Inner = parseOrExpr();
      if (!Inner) {
        return nullptr;
      }
      if (!match(TokenKind::RParen)) {
        emitSyntaxError(peek().Range, "Ожидалась закрывающая ')'");
        return nullptr;
      }
      Inner->Range = {OpenTok.Range.Start, previous().Range.End};
      Primary = std::move(Inner);
    } else {
      emitSyntaxError(peek().Range, "Ожидалось выражение");
      return nullptr;
    }

    if (Primary && check(TokenKind::LBracket)) {
      emitSyntaxError(peek().Range,
                      "Индексация '[...]' допустима только после имени или "
                      "вызова");
      return nullptr;
    }
    return Primary;
  }

  ExprPtr parseIdentExpr() {
    const Token &Tok = advance();
    return makeExpr(Tok.Range, IdentExpr{Tok.Text});
  }

  ExprPtr parseCallExpr() {
    const Token &NameTok = advance();
    advance(); // '('
    CallExpr Call;
    Call.Callee = NameTok.Text;
    Call.CalleeRange = NameTok.Range;
    if (!check(TokenKind::RParen)) {
      for (;;) {
        ExprPtr Arg = parseOrExpr();
        if (!Arg) {
          return nullptr;
        }
        Call.Args.push_back(std::move(Arg));
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
    }
    if (!match(TokenKind::RParen)) {
      emitSyntaxError(peek().Range, "Ожидалась закрывающая ')' в вызове");
      return nullptr;
    }
    const SourceRange Span{NameTok.Range.Start, previous().Range.End};
    return makeExpr(Span, std::move(Call));
  }

  const std::vector<Token> *Tokens = nullptr;
  std::size_t Pos = 0;
  std::vector<Diagnostic> Diagnostics;
  bool HasSyntaxErrors = false;
  std::size_t SyntaxErrorCount = 0;
  Program Ast;
  bool AstNodeLimitReported = false;
  std::size_t CallDepth = 0;
};
bool numbersEqual(double Lhs, double Rhs) noexcept {
  return (isNaValue(Lhs) && isNaValue(Rhs)) || Lhs == Rhs;
}

bool exprEqual(const Expr &Lhs, const Expr &Rhs);

bool exprPtrEqual(const ExprPtr &Lhs, const ExprPtr &Rhs) {
  if (!Lhs && !Rhs) {
    return true;
  }
  if (!Lhs || !Rhs) {
    return false;
  }
  return exprEqual(*Lhs, *Rhs);
}

bool exprEqual(const Expr &Lhs, const Expr &Rhs) {
  if (Lhs.Node.index() != Rhs.Node.index()) {
    return false;
  }
  return std::visit(
      [&](const auto &LeftInner) -> bool {
        using T = std::decay_t<decltype(LeftInner)>;
        const auto &RightInner = std::get<T>(Rhs.Node);
        if constexpr (std::is_same_v<T, NumberLitExpr>) {
          return numbersEqual(LeftInner.Value, RightInner.Value) &&
                 LeftInner.IsInteger == RightInner.IsInteger;
        } else if constexpr (std::is_same_v<T, BoolLitExpr>) {
          return LeftInner.Value == RightInner.Value;
        } else if constexpr (std::is_same_v<T, IdentExpr>) {
          return LeftInner.Name == RightInner.Name;
        } else if constexpr (std::is_same_v<T, OffsetExpr>) {
          if (!exprPtrEqual(LeftInner.Target, RightInner.Target) ||
              LeftInner.HasUnaryMinus != RightInner.HasUnaryMinus ||
              LeftInner.OffsetIsIdent != RightInner.OffsetIsIdent) {
            return false;
          }
          return LeftInner.OffsetIsIdent
                     ? (LeftInner.OffsetIdent == RightInner.OffsetIdent)
                     : (LeftInner.OffsetInt == RightInner.OffsetInt);
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
          return LeftInner.Op == RightInner.Op &&
                 exprPtrEqual(LeftInner.Operand, RightInner.Operand);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
          return LeftInner.Op == RightInner.Op &&
                 exprPtrEqual(LeftInner.Left, RightInner.Left) &&
                 exprPtrEqual(LeftInner.Right, RightInner.Right);
        } else {
          if (LeftInner.Callee != RightInner.Callee ||
              LeftInner.Args.size() != RightInner.Args.size()) {
            return false;
          }
          for (std::size_t I = 0; I < LeftInner.Args.size(); ++I) {
            if (!exprPtrEqual(LeftInner.Args[I], RightInner.Args[I])) {
              return false;
            }
          }
          return true;
        }
      },
      Lhs.Node);
}

bool paramEqual(const ParamStmt &Lhs, const ParamStmt &Rhs) {
  if (Lhs.Name != Rhs.Name ||
      !numbersEqual(Lhs.DefaultValue, Rhs.DefaultValue) ||
      Lhs.DefaultIsInteger != Rhs.DefaultIsInteger ||
      Lhs.HasRange != Rhs.HasRange || Lhs.HasStep != Rhs.HasStep) {
    return false;
  }
  if (Lhs.HasRange) {
    if (!numbersEqual(Lhs.RangeMin, Rhs.RangeMin) ||
        !numbersEqual(Lhs.RangeMax, Rhs.RangeMax) ||
        Lhs.RangeMinIsInt != Rhs.RangeMinIsInt ||
        Lhs.RangeMaxIsInt != Rhs.RangeMaxIsInt) {
      return false;
    }
  }
  if (Lhs.HasStep) {
    if (!numbersEqual(Lhs.StepValue, Rhs.StepValue) ||
        Lhs.StepIsInteger != Rhs.StepIsInteger ||
        Lhs.StepRawText != Rhs.StepRawText) {
      return false;
    }
  }
  return true;
}

bool actionEqual(const ActionNode &Lhs, const ActionNode &Rhs) {
  return Lhs.Kind == Rhs.Kind && Lhs.Unit == Rhs.Unit &&
         exprPtrEqual(Lhs.SizeExpr, Rhs.SizeExpr) &&
         Lhs.OrderKind == Rhs.OrderKind &&
         exprPtrEqual(Lhs.PriceExpr, Rhs.PriceExpr) && Lhs.Tif == Rhs.Tif;
}

bool becauseEqual(const BecauseClause &Lhs, const BecauseClause &Rhs) {
  if (Lhs.Present != Rhs.Present || Lhs.Parts.size() != Rhs.Parts.size()) {
    return false;
  }
  for (std::size_t I = 0; I < Lhs.Parts.size(); ++I) {
    const auto &LeftPart = Lhs.Parts[I];
    const auto &RightPart = Rhs.Parts[I];
    if (LeftPart.IsExpr != RightPart.IsExpr) {
      return false;
    }
    if (LeftPart.IsExpr) {
      if (!exprPtrEqual(LeftPart.Expression, RightPart.Expression)) {
        return false;
      }
    } else if (LeftPart.LiteralText != RightPart.LiteralText) {
      return false;
    }
  }
  return true;
}

bool ruleEqual(const RuleStmt &Lhs, const RuleStmt &Rhs) {
  return Lhs.Name == Rhs.Name && exprPtrEqual(Lhs.Condition, Rhs.Condition) &&
         actionEqual(Lhs.Action, Rhs.Action) &&
         becauseEqual(Lhs.Because, Rhs.Because);
}

} // namespace

ParseResult parse(std::string_view Source) {
  LexResult LexRes = internal::lex(Source);
  if (LexRes.HasFatalError) {
    ParseResult Result;
    Result.Diagnostics = std::move(LexRes.Diagnostics);
    Result.HasSyntaxErrors = true;
    return Result;
  }
  bool LexHasSyntax = false;
  for (const auto &Diag : LexRes.Diagnostics) {
    if (Diag.Code == "E-SYN") {
      LexHasSyntax = true;
      break;
    }
  }
  Parser State(LexRes.Tokens, std::move(LexRes.Diagnostics), LexHasSyntax);
  return State.parseProgram();
}

bool astEqual(const Program &Left, const Program &Right) {
  if (Left.Params.size() != Right.Params.size() ||
      Left.Rules.size() != Right.Rules.size()) {
    return false;
  }
  for (std::size_t I = 0; I < Left.Params.size(); ++I) {
    if (!paramEqual(Left.Params[I], Right.Params[I])) {
      return false;
    }
  }
  for (std::size_t I = 0; I < Left.Rules.size(); ++I) {
    if (!ruleEqual(Left.Rules[I], Right.Rules[I])) {
      return false;
    }
  }
  return true;
}

} // namespace dsl
