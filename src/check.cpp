#include "dsl/check.hpp"

#include "dsl/ast.hpp"
#include "dsl/types.hpp"
#include "internal/analysis.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dsl {

void sortDiagnostics(std::vector<Diagnostic> &Diagnostics) {
  std::stable_sort(
      Diagnostics.begin(), Diagnostics.end(),
      [](const Diagnostic &Lhs, const Diagnostic &Rhs) {
        return std::tie(Lhs.Range.Start.Line, Lhs.Range.Start.Column,
                        Lhs.Range.End.Line, Lhs.Range.End.Column, Lhs.Code) <
               std::tie(Rhs.Range.Start.Line, Rhs.Range.Start.Column,
                        Rhs.Range.End.Line, Rhs.Range.End.Column, Rhs.Code);
      });
}

CheckResult check(std::string_view Source,
                  const std::map<std::string, double> &ParamOverrides) {
  CheckResult Result;
  ParseResult Parsed = parse(Source);
  Result.Diagnostics = std::move(Parsed.Diagnostics);
  if (Parsed.HasSyntaxErrors) {
    sortDiagnostics(Result.Diagnostics);
    return Result;
  }

  internal::AnalysisState State = internal::analyzeSemantics(
      Parsed.Ast, ParamOverrides, Result.Diagnostics);

  bool HasSyntaxError = false;
  for (const auto &Diag : Result.Diagnostics) {
    if (Diag.Code == "E-SYN") {
      HasSyntaxError = true;
      break;
    }
  }
  if (HasSyntaxError) {
    sortDiagnostics(Result.Diagnostics);
    return Result;
  }

  Result.Lookahead = State.Lookahead;
  Result.Params = std::move(State.ParamDecls);
  internal::runLinter(Parsed.Ast, Source, State, Result.Diagnostics);
  sortDiagnostics(Result.Diagnostics);
  return Result;
}

} // namespace dsl
