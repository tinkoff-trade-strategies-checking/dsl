#pragma once

#include "dsl/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dsl::internal {

enum class TokenKind {
  // 23 keywords.
  KwParam,
  KwRange,
  KwStep,
  KwRule,
  KwWhen,
  KwThen,
  KwBecause,
  KwBuy,
  KwSell,
  KwCancel,
  KwLots,
  KwAll,
  KwMarket,
  KwLimit,
  KwStop,
  KwTakeProfit,
  KwDay,
  KwGtc,
  KwAnd,
  KwOr,
  KwNot,
  KwTrue,
  KwFalse,

  // Identifiers and literals.
  Ident,
  IntLit,
  FloatLit,
  StringLit,

  // Operators and delimiters.
  Plus,
  Minus,
  Star,
  Slash,
  EqEq,
  BangEq,
  Lt,
  LtEq,
  Gt,
  GtEq,
  Eq,
  DotDot,
  Percent,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,

  Eof
};

/// Single lexical token with 1-based half-open `SourceRange`.
struct Token {
  TokenKind Kind = TokenKind::Eof;
  std::string Text;
  double NumericValue = 0.0;
  std::int64_t IntValue = 0;
  SourceRange Range;
};

/// Output of `lex` or `lexInlineExpr`.
struct LexResult {
  std::vector<Token> Tokens;
  std::vector<Diagnostic> Diagnostics;
  bool HasFatalError = false;
};

/// Lexes full strategy source text; caller must check UTF-8/size first or rely
/// on `lex` setting `HasFatalError` on invalid UTF-8.
LexResult lex(std::string_view Source);

/// Lexes an inline `{ expr }` slice from `because`, tracking 1-based
/// coordinates starting at `Origin` and rejecting `#` comments as `E-SYN`.
LexResult lexInlineExpr(std::string_view ExprText, SourcePos Origin);

/// Returns true iff `Text` is valid UTF-8.
bool isValidUtf8(std::string_view Text);

} // namespace dsl::internal
