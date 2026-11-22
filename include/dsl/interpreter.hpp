#pragma once

#include "dsl/types.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsl {

/// Compiled strategy state with per-instrument ring buffers and DAG nodes.
class StrategyInstance {
public:
  struct Impl;

  explicit StrategyInstance(std::unique_ptr<Impl> State);
  StrategyInstance(const StrategyInstance &) = delete;
  StrategyInstance &operator=(const StrategyInstance &) = delete;
  StrategyInstance(StrategyInstance &&) noexcept;
  StrategyInstance &operator=(StrategyInstance &&) noexcept;
  ~StrategyInstance();

  /// Number of instruments allocated for this run.
  std::size_t instrumentCount() const;

  /// Number of unique deduplicated indicator/cross DAG nodes.
  std::size_t dagNodeCount() const;

  /// Maximum bar offset across all series and DAG nodes.
  std::size_t maxLookback() const;

  /// Resolved parameter metadata (`CheckResult::Params`).
  const std::vector<ParamDecl> &params() const;

  /// Advances one instrument by `CurrentBar` and evaluates rules when not in
  /// warmup.
  StepOutcome step(std::size_t InstrumentIdx, const Bar &CurrentBar,
                   const PortfolioSnapshot &Portfolio, bool IsWarmup);

  /// Requests cooperative cancellation between `step` calls (NFR-7).
  void requestCancel() noexcept;

  /// Returns true if cooperative cancellation was requested.
  bool isCancelled() const noexcept;

private:
  std::unique_ptr<Impl> Pimpl;
};

/// Output of `compile`.
struct CompileResult {
  std::unique_ptr<StrategyInstance> Instance;
  std::vector<Diagnostic> Diagnostics;
  bool Ok = false;
};

/// Re-runs static checks with `ParamOverrides`, substitutes parameters,
/// builds the indicator DAG, and allocates buffers for `InstrumentCount`.
CompileResult compile(std::string_view Source,
                      const std::map<std::string, double> &ParamOverrides,
                      std::size_t InstrumentCount);

/// Convenience wrapper forwarding to `Instance.step(...)`.
StepOutcome step(StrategyInstance &Instance, std::size_t InstrumentIdx,
                 const Bar &CurrentBar, const PortfolioSnapshot &Portfolio,
                 bool IsWarmup);

/// Formats a numeric value for `because` templates (decimal `HALF_UP` to 2
/// places, comma separator, `-0 -> 0`, `н/д` for non-finite values).
std::string formatBecauseNumber(double Value);

} // namespace dsl
