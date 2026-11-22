#pragma once

#include "dsl/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dsl {

enum class ValueType { Number, Bool, Error };

enum class UnaryOp { Neg, Not };

enum class BinaryOp { Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div };

enum class SizeUnit { Lots, PercentEquity, PercentCash, PercentPosition, All };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

/// Numeric literal (`INT_LIT` or `FLOAT_LIT`).
struct NumberLitExpr {
  double Value = 0.0;
  bool IsInteger = false;
  std::string RawText;
};

/// Boolean literal (`true` or `false`).
struct BoolLitExpr {
  bool Value = false;
};

/// Bare identifier reference (series, portfolio variable, or param).
struct IdentExpr {
  std::string Name;
};

/// Postfix bar-offset access `(ident | call)[[-](integer | ident)]`.
struct OffsetExpr {
  ExprPtr Target;
  bool HasUnaryMinus = false;
  bool OffsetIsIdent = false;
  std::string OffsetIdent;
  std::int64_t OffsetInt = 0;
  // Source span of the offset inside `[...]` (including unary `-` if any).
  SourceRange OffsetRange;
};

/// Unary prefix expression (`-x` or `not x`).
struct UnaryExpr {
  UnaryOp Op = UnaryOp::Neg;
  ExprPtr Operand;
};

/// Binary expression (`or`, `and`, comparison, or arithmetic).
struct BinaryExpr {
  BinaryOp Op = BinaryOp::Add;
  ExprPtr Left;
  ExprPtr Right;
};

/// Function call `callee(arg0, ...)`.
struct CallExpr {
  std::string Callee;
  SourceRange CalleeRange;
  std::vector<ExprPtr> Args;
};

using ExprNode = std::variant<NumberLitExpr, BoolLitExpr, IdentExpr, OffsetExpr,
                              UnaryExpr, BinaryExpr, CallExpr>;

/// AST node for any expression; `ResolvedType` is populated in Phase 2.
struct Expr {
  SourceRange Range;
  ValueType ResolvedType = ValueType::Error;
  ExprNode Node;
};

/// Segment of a `because` template string: decoded text or `{expr}`.
struct TemplatePart {
  bool IsExpr = false;
  std::string LiteralText;
  ExprPtr Expression;
  SourceRange Range;
};

/// `because "..."` clause attached to a rule.
struct BecauseClause {
  bool Present = false;
  std::vector<TemplatePart> Parts;
  SourceRange Range;
};

/// Top-level `param` declaration.
struct ParamStmt {
  std::string Name;
  SourceRange NameRange;
  double DefaultValue = 0.0;
  bool DefaultIsInteger = false;
  std::string DefaultRawText;
  SourceRange DefaultRange;
  bool HasRange = false;
  double RangeMin = 0.0;
  double RangeMax = 0.0;
  bool RangeMinIsInt = false;
  bool RangeMaxIsInt = false;
  std::string RangeMinText;
  std::string RangeMaxText;
  SourceRange RangeMinRange;
  SourceRange RangeMaxRange;
  bool HasStep = false;
  double StepValue = 0.0;
  bool StepIsInteger = false;
  std::string StepRawText;
  SourceRange StepRange;
  SourceRange Range;
};

/// Action inside a `rule ... then <action>` clause.
struct ActionNode {
  ActionKind Kind = ActionKind::Buy;
  ExprPtr SizeExpr;
  SizeUnit Unit = SizeUnit::Lots;
  SourceRange SizeRange;
  OrderType OrderKind = OrderType::Market;
  ExprPtr PriceExpr;
  SourceRange OrderRange;
  bool ExplicitOrder = false;
  TimeInForce Tif = TimeInForce::Gtc;
  bool ExplicitTif = false;
  SourceRange Range;
};

/// Top-level `rule` statement; `Name` holds unescaped text before trimming.
struct RuleStmt {
  std::string Name;
  SourceRange NameRange;
  ExprPtr Condition;
  ActionNode Action;
  BecauseClause Because;
  SourceRange Range;
};

/// Parsed strategy program AST.
struct Program {
  std::vector<ParamStmt> Params;
  std::vector<RuleStmt> Rules;
  std::size_t AstNodeCount = 0;
};

/// Output of Phase 1 (`parse`).
struct ParseResult {
  Program Ast;
  std::vector<Diagnostic> Diagnostics;
  bool HasSyntaxErrors = false;
};

/// Structural AST equality ignoring `SourceRange`, `ResolvedType`, and
/// explicit default `market`/`gtc` markers.
bool astEqual(const Program &Left, const Program &Right);

} // namespace dsl
