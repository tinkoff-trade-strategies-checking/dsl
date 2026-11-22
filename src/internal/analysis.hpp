#pragma once

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dsl::internal {

/// Resolved parameter metadata computed in Phase 2.
struct ResolvedParam {
  std::string Name;
  double EffectiveValue = 0.0;
  bool IsInteger = false;
  bool HasRange = false;
  double RangeMin = 0.0;
  double RangeMax = 0.0;
  bool Valid = true;
};

/// Shared analysis state passed from Phase 2/3 (`analyzeSemantics`) to
/// Phase 4 (`runLinter`) and `check`.
struct AnalysisState {
  std::map<std::string, ResolvedParam> ParamMap;
  std::vector<ParamDecl> ParamDecls;
  LookaheadLevel Lookahead = LookaheadLevel::Ok;
  std::size_t UniqueDagNodes = 0;
  bool HasPhase2Errors = false;
};

/// Returns true iff `Name` is a built-in series, portfolio variable, or
/// built-in function name.
bool isBuiltinName(std::string_view Name);

/// Runs Phase 2 (name resolution, `param` & override validation, rule/because
/// checks, expression typing) and Phase 3 (lookahead `E-LA01`, `E-LA02`,
/// `W-LA01`), populating `Expr::ResolvedType` in `Ast`.
AnalysisState
analyzeSemantics(Program &Ast,
                 const std::map<std::string, double> &ParamOverrides,
                 std::vector<Diagnostic> &Diagnostics);

/// Runs Phase 4 (`L001` magic-constant linter and auto-fixes) in pre-order.
void runLinter(const Program &Ast, std::string_view Source,
               const AnalysisState &State,
               std::vector<Diagnostic> &Diagnostics);

} // namespace dsl::internal
