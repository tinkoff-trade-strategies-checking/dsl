#include "internal/lexer.hpp"

#include "dsl/types.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dsl::internal {
namespace {

struct KeywordEntry {
  std::string_view Text;
  TokenKind Kind;
};

constexpr KeywordEntry kKeywords[] = {
    {"and", TokenKind::KwAnd},
    {"because", TokenKind::KwBecause},
    {"buy", TokenKind::KwBuy},
    {"cancel", TokenKind::KwCancel},
    {"day", TokenKind::KwDay},
    {"false", TokenKind::KwFalse},
    {"gtc", TokenKind::KwGtc},
    {"limit", TokenKind::KwLimit},
    {"lots", TokenKind::KwLots},
    {"market", TokenKind::KwMarket},
    {"not", TokenKind::KwNot},
    {"or", TokenKind::KwOr},
    {"param", TokenKind::KwParam},
    {"range", TokenKind::KwRange},
    {"rule", TokenKind::KwRule},
    {"sell", TokenKind::KwSell},
    {"step", TokenKind::KwStep},
    {"stop", TokenKind::KwStop},
    {"take_profit", TokenKind::KwTakeProfit},
    {"then", TokenKind::KwThen},
    {"true", TokenKind::KwTrue},
    {"when", TokenKind::KwWhen},
    {"all", TokenKind::KwAll},
};

std::size_t utf8CodePointLength(std::uint8_t LeadByte) noexcept {
  if (LeadByte < 0x80U) {
    return 1;
  }
  if ((LeadByte & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((LeadByte & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((LeadByte & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

bool isIdentStart(char Ch) noexcept {
  const auto U = static_cast<unsigned char>(Ch);
  return (U >= 'a' && U <= 'z') || U == '_';
}

bool isIdentContinue(char Ch) noexcept {
  const auto U = static_cast<unsigned char>(Ch);
  return isIdentStart(Ch) || (U >= '0' && U <= '9');
}

bool isAsciiDigit(char Ch) noexcept {
  const auto U = static_cast<unsigned char>(Ch);
  return U >= '0' && U <= '9';
}

std::optional<TokenKind> lookupKeyword(std::string_view Word) noexcept {
  for (const auto &Entry : kKeywords) {
    if (Entry.Text == Word) {
      return Entry.Kind;
    }
  }
  return std::nullopt;
}

bool hasNonZeroIntegerPart(std::string_view NumText) noexcept {
  for (char Ch : NumText) {
    if (Ch == '.') {
      return false;
    }
    if (Ch != '0') {
      return true;
    }
  }
  return false;
}

class Lexer {
public:
  Lexer(std::string_view SourceText, SourcePos OriginPos, bool InlineMode)
      : Input(SourceText), CurLine(OriginPos.Line), CurCol(OriginPos.Column),
        IsInline(InlineMode) {}

  LexResult run() {
    while (CurPos < Input.size()) {
      const char Ch = Input[CurPos];
      if (Ch == ' ' || Ch == '\t') {
        ++CurPos;
        ++CurCol;
        continue;
      }
      if (Ch == '\r' || Ch == '\n') {
        consumeLineBreak();
        continue;
      }
      if (Ch == '#') {
        if (IsInline) {
          const SourcePos StartPos = currentPos();
          while (CurPos < Input.size() && Input[CurPos] != '\n' &&
                 Input[CurPos] != '\r') {
            advanceCodePoint();
          }
          emitSyntaxError({StartPos, currentPos()},
                          "Комментарии '#' запрещены внутри выражения в "
                          "because");
        } else {
          while (CurPos < Input.size() && Input[CurPos] != '\n' &&
                 Input[CurPos] != '\r') {
            advanceCodePoint();
          }
        }
        continue;
      }
      if (Ch == '"') {
        if (IsInline) {
          const SourcePos StartPos = currentPos();
          advanceCodePoint();
          emitSyntaxError({StartPos, currentPos()},
                          "Строковые литералы запрещены внутри выражения в "
                          "because");
        } else {
          lexStringLiteral();
        }
        continue;
      }
      if (isIdentStart(Ch)) {
        lexIdentifierOrKeyword();
        continue;
      }
      if (isAsciiDigit(Ch)) {
        lexNumberLiteral();
        continue;
      }
      lexOperatorOrPunctuation();
    }

    const SourcePos EofPos = currentPos();
    Output.Tokens.push_back(
        Token{TokenKind::Eof, "", 0.0, 0, {EofPos, EofPos}});
    return Output;
  }

private:
  SourcePos currentPos() const noexcept { return {CurLine, CurCol}; }

  void advanceCodePoint() noexcept {
    if (CurPos >= Input.size()) {
      return;
    }
    const auto ByteLen =
        utf8CodePointLength(static_cast<std::uint8_t>(Input[CurPos]));
    const std::size_t Step =
        (CurPos + ByteLen <= Input.size()) ? ByteLen : (Input.size() - CurPos);
    CurPos += Step;
    ++CurCol;
  }

  void consumeLineBreak() noexcept {
    if (Input[CurPos] == '\r' && CurPos + 1 < Input.size() &&
        Input[CurPos + 1] == '\n') {
      CurPos += 2;
    } else {
      ++CurPos;
    }
    ++CurLine;
    CurCol = 1;
  }

  void emitSyntaxError(SourceRange Range, std::string Message) {
    if (Output.Diagnostics.size() >= kMaxSyntaxErrors) {
      return;
    }
    Output.Diagnostics.push_back(Diagnostic{
        "E-SYN", DiagnosticSeverity::Error, Range, std::move(Message), {}});
  }

  void lexIdentifierOrKeyword() {
    const SourcePos StartPos = currentPos();
    const std::size_t StartByte = CurPos;
    while (CurPos < Input.size() && isIdentContinue(Input[CurPos])) {
      ++CurPos;
      ++CurCol;
    }
    const std::string_view Word = Input.substr(StartByte, CurPos - StartByte);
    const SourcePos EndPos = currentPos();
    const TokenKind Kind = lookupKeyword(Word).value_or(TokenKind::Ident);
    Output.Tokens.push_back(
        Token{Kind, std::string(Word), 0.0, 0, {StartPos, EndPos}});
  }

  void lexNumberLiteral() {
    const SourcePos StartPos = currentPos();
    const std::size_t StartByte = CurPos;
    while (CurPos < Input.size() && isAsciiDigit(Input[CurPos])) {
      ++CurPos;
      ++CurCol;
    }
    bool IsFloat = false;
    if (CurPos + 1 < Input.size() && Input[CurPos] == '.' &&
        isAsciiDigit(Input[CurPos + 1])) {
      IsFloat = true;
      ++CurPos;
      ++CurCol;
      while (CurPos < Input.size() && isAsciiDigit(Input[CurPos])) {
        ++CurPos;
        ++CurCol;
      }
    }
    const SourcePos EndPos = currentPos();
    const std::string_view NumView =
        Input.substr(StartByte, CurPos - StartByte);

    double ParsedDouble = 0.0;
    const auto DoubleRes = std::from_chars(
        NumView.data(), NumView.data() + NumView.size(), ParsedDouble);
    bool Overflow = false;
    if (DoubleRes.ec == std::errc::result_out_of_range) {
      if (!IsFloat || hasNonZeroIntegerPart(NumView)) {
        Overflow = true;
      } else {
        ParsedDouble = 0.0;
      }
    } else if (DoubleRes.ec != std::errc{} || !std::isfinite(ParsedDouble)) {
      Overflow = true;
    }

    if (Overflow) {
      emitSyntaxError({StartPos, EndPos},
                      "Числовой литерал не представим конечным double");
      ParsedDouble = 0.0;
    }

    std::int64_t ParsedInt = 0;
    if (!IsFloat) {
      const auto IntRes = std::from_chars(
          NumView.data(), NumView.data() + NumView.size(), ParsedInt);
      if (IntRes.ec == std::errc::result_out_of_range) {
        ParsedInt = std::numeric_limits<std::int64_t>::max();
      }
    }

    Token Tok;
    Tok.Kind = IsFloat ? TokenKind::FloatLit : TokenKind::IntLit;
    Tok.Text = std::string(NumView);
    Tok.NumericValue = ParsedDouble;
    Tok.IntValue = ParsedInt;
    Tok.Range = {StartPos, EndPos};
    Output.Tokens.push_back(std::move(Tok));
  }

  void lexStringLiteral() {
    const SourcePos StartPos = currentPos();
    ++CurPos;
    ++CurCol;
    const std::size_t BodyStart = CurPos;

    while (CurPos < Input.size()) {
      const char Ch = Input[CurPos];
      if (Ch == '"') {
        const std::string_view Body =
            Input.substr(BodyStart, CurPos - BodyStart);
        ++CurPos;
        ++CurCol;
        const SourcePos EndPos = currentPos();
        Token Tok;
        Tok.Kind = TokenKind::StringLit;
        Tok.Text = std::string(Body);
        Tok.Range = {StartPos, EndPos};
        Output.Tokens.push_back(std::move(Tok));
        return;
      }
      if (Ch == '\n' || Ch == '\r') {
        emitSyntaxError({StartPos, currentPos()},
                        "Незакрытый строковый литерал");
        return;
      }
      if (Ch == '\t') {
        const SourcePos TabStart = currentPos();
        ++CurPos;
        ++CurCol;
        emitSyntaxError({TabStart, currentPos()},
                        "Сырой символ табуляции запрещён в строковом литерале");
        continue;
      }
      if (Ch == '\\') {
        const SourcePos EscStart = currentPos();
        if (CurPos + 1 >= Input.size() || Input[CurPos + 1] == '\n' ||
            Input[CurPos + 1] == '\r') {
          ++CurPos;
          ++CurCol;
          emitSyntaxError({StartPos, currentPos()},
                          "Незакрытый строковый литерал");
          return;
        }
        const char NextCh = Input[CurPos + 1];
        if (NextCh == '"' || NextCh == '\\') {
          CurPos += 2;
          CurCol += 2;
        } else {
          ++CurPos;
          ++CurCol;
          advanceCodePoint();
          emitSyntaxError({EscStart, currentPos()},
                          "Недопустимая escape-последовательность в строке");
        }
        continue;
      }
      advanceCodePoint();
    }

    emitSyntaxError({StartPos, currentPos()}, "Незакрытый строковый литерал");
  }

  void lexOperatorOrPunctuation() {
    const SourcePos StartPos = currentPos();
    const char Ch = Input[CurPos];
    char NextCh = '\0';
    if (CurPos + 1 < Input.size()) {
      NextCh = Input[CurPos + 1];
    }

    auto emitToken = [&](TokenKind Kind, std::size_t Len) {
      const std::string_view Lexeme = Input.substr(CurPos, Len);
      CurPos += Len;
      CurCol += Len;
      Output.Tokens.push_back(
          Token{Kind, std::string(Lexeme), 0.0, 0, {StartPos, currentPos()}});
    };

    if (Ch == '=' && NextCh == '=') {
      emitToken(TokenKind::EqEq, 2);
      return;
    }
    if (Ch == '!' && NextCh == '=') {
      emitToken(TokenKind::BangEq, 2);
      return;
    }
    if (Ch == '<' && NextCh == '=') {
      emitToken(TokenKind::LtEq, 2);
      return;
    }
    if (Ch == '>' && NextCh == '=') {
      emitToken(TokenKind::GtEq, 2);
      return;
    }
    if (Ch == '.' && NextCh == '.') {
      emitToken(TokenKind::DotDot, 2);
      return;
    }

    switch (Ch) {
    case '+':
      emitToken(TokenKind::Plus, 1);
      return;
    case '-':
      emitToken(TokenKind::Minus, 1);
      return;
    case '*':
      emitToken(TokenKind::Star, 1);
      return;
    case '/':
      emitToken(TokenKind::Slash, 1);
      return;
    case '<':
      emitToken(TokenKind::Lt, 1);
      return;
    case '>':
      emitToken(TokenKind::Gt, 1);
      return;
    case '=':
      emitToken(TokenKind::Eq, 1);
      return;
    case '%':
      emitToken(TokenKind::Percent, 1);
      return;
    case '(':
      emitToken(TokenKind::LParen, 1);
      return;
    case ')':
      emitToken(TokenKind::RParen, 1);
      return;
    case '[':
      emitToken(TokenKind::LBracket, 1);
      return;
    case ']':
      emitToken(TokenKind::RBracket, 1);
      return;
    case ',':
      emitToken(TokenKind::Comma, 1);
      return;
    default:
      break;
    }

    advanceCodePoint();
    emitSyntaxError({StartPos, currentPos()}, "Неожиданный символ в тексте");
  }

  std::string_view Input;
  std::size_t CurPos = 0;
  std::size_t CurLine = 1;
  std::size_t CurCol = 1;
  bool IsInline = false;
  LexResult Output;
};

bool isContByte(std::uint8_t Byte) noexcept {
  return Byte >= 0x80U && Byte <= 0xBFU;
}

LexResult makeFatalError(SourcePos Pos, std::string Message) {
  LexResult Res;
  Res.HasFatalError = true;
  Diagnostic Diag;
  Diag.Code = "E-SYN";
  Diag.Severity = DiagnosticSeverity::Error;
  Diag.Range = {Pos, Pos};
  Diag.Message = std::move(Message);
  Res.Diagnostics.push_back(std::move(Diag));
  return Res;
}

} // namespace

bool isValidUtf8(std::string_view Text) {
  std::size_t Index = 0;
  const std::size_t Size = Text.size();
  while (Index < Size) {
    const auto B0 = static_cast<std::uint8_t>(Text[Index]);
    if (B0 <= 0x7FU) {
      ++Index;
      continue;
    }
    if (B0 >= 0xC2U && B0 <= 0xDFU) {
      if (Index + 1 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      if (!isContByte(B1)) {
        return false;
      }
      Index += 2;
      continue;
    }
    if (B0 == 0xE0U) {
      if (Index + 2 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      if (B1 < 0xA0U || B1 > 0xBFU || !isContByte(B2)) {
        return false;
      }
      Index += 3;
      continue;
    }
    if ((B0 >= 0xE1U && B0 <= 0xECU) || (B0 >= 0xEEU && B0 <= 0xEFU)) {
      if (Index + 2 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      if (!isContByte(B1) || !isContByte(B2)) {
        return false;
      }
      Index += 3;
      continue;
    }
    if (B0 == 0xEDU) {
      if (Index + 2 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      if (B1 < 0x80U || B1 > 0x9FU || !isContByte(B2)) {
        return false;
      }
      Index += 3;
      continue;
    }
    if (B0 == 0xF0U) {
      if (Index + 3 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      const auto B3 = static_cast<std::uint8_t>(Text[Index + 3]);
      if (B1 < 0x90U || B1 > 0xBFU || !isContByte(B2) || !isContByte(B3)) {
        return false;
      }
      Index += 4;
      continue;
    }
    if (B0 >= 0xF1U && B0 <= 0xF3U) {
      if (Index + 3 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      const auto B3 = static_cast<std::uint8_t>(Text[Index + 3]);
      if (!isContByte(B1) || !isContByte(B2) || !isContByte(B3)) {
        return false;
      }
      Index += 4;
      continue;
    }
    if (B0 == 0xF4U) {
      if (Index + 3 >= Size) {
        return false;
      }
      const auto B1 = static_cast<std::uint8_t>(Text[Index + 1]);
      const auto B2 = static_cast<std::uint8_t>(Text[Index + 2]);
      const auto B3 = static_cast<std::uint8_t>(Text[Index + 3]);
      if (B1 < 0x80U || B1 > 0x8FU || !isContByte(B2) || !isContByte(B3)) {
        return false;
      }
      Index += 4;
      continue;
    }
    return false;
  }
  return true;
}

LexResult lex(std::string_view Source) {
  if (Source.size() > kMaxSourceBytes) {
    const std::string Msg = "Превышен максимальный размер текста (64 КиБ)";
    return makeFatalError({0, 0}, Msg);
  }
  if (!isValidUtf8(Source)) {
    const std::string Msg = "Текст стратегии не является валидным UTF-8";
    return makeFatalError({0, 0}, Msg);
  }
  Lexer LexerState(Source, {1, 1}, false);
  return LexerState.run();
}

LexResult lexInlineExpr(std::string_view ExprText, SourcePos Origin) {
  if (!isValidUtf8(ExprText)) {
    const std::string Msg = "Выражение в because не является валидным UTF-8";
    return makeFatalError(Origin, Msg);
  }
  Lexer LexerState(ExprText, Origin, true);
  return LexerState.run();
}

} // namespace dsl::internal
