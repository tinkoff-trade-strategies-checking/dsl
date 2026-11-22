#pragma once

#include "dsl/ast.hpp"
#include "dsl/types.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dsl {

/// Phase 1: validates UTF-8 and size limit, lexes, and parses `Source`.
ParseResult parse(std::string_view Source);

/// Canonical pretty-printer for a syntactically valid `Program` AST.
std::string print(const Program &Ast);

/// Runs the full 4-phase static analysis pipeline (`parse`, semantics,
/// lookahead, linter) with optional `ParamOverrides`.
CheckResult check(std::string_view Source,
                  const std::map<std::string, double> &ParamOverrides = {});

/// Applies non-overlapping `TextEdit`s (sorted back-to-front) to `Source`.
std::string applyEdits(std::string_view Source,
                       const std::vector<TextEdit> &Edits);

/// Deterministically sorts diagnostics by `(Start, End, Code)` (INV-4).
void sortDiagnostics(std::vector<Diagnostic> &Diagnostics);

} // namespace dsl
