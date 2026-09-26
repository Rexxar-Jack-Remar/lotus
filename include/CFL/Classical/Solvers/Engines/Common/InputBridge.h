// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/Classical/Solvers/Engines/Common/Reachability.h"
#include "CFL/Classical/Solvers/Engines/Skewed/InputBridge.h"
namespace lotus::cfl::classical::common {
template <class ExternalSymbol, class Hash = std::hash<ExternalSymbol>>
using InputBridge = skewed::InputBridge<ExternalSymbol, Hash>;

// Export using the same bridge/symbol map that imported the input. The callback
// receives original vertex IDs and original external labels, never dense IDs
// or SCC representatives. It should write to the derived relation, NOT to the
// terminal input graph. Partial target results must not be exposed as
// saturation.
template <class ExternalSymbol, class Hash, class Sink>
void exportFacts(const InputBridge<ExternalSymbol, Hash> &input,
                 const Reachability &result, Sink &&sink,
                 std::size_t max_facts = 0) {
  // Validate every label before making any destination changes.
  for (Symbol s : result.outputSymbols())
    (void)input.externalSymbol(s);
  result.forEachFact(
      [&](const Fact &f) {
        sink(f.source, input.externalSymbol(f.symbol), f.target);
      },
      max_facts);
}
} // namespace lotus::cfl::classical::common
