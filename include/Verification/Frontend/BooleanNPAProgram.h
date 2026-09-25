#pragma once

#include "Dataflow/NPA/Core/Expr/Expressions.h"
#include "Verification/Frontend/BooleanProgram.h"

#include <string>
#include <utility>
#include <vector>

namespace lotus {
namespace verification {
namespace frontend {

struct BooleanNPAProgram {
  using Domain = npa::PredicateRelationDomain;
  using Equation = std::pair<npa::Symbol, npa::E0<Domain>>;

  LoweredBooleanProgram lowered;
  std::vector<Equation> equations;
  std::vector<npa::Symbol> summary_symbols;
  std::vector<npa::Symbol> error_summary_symbols;
  std::vector<npa::Symbol> entry_reach_symbols;
  std::vector<std::vector<npa::Symbol>> node_symbols;
  std::vector<std::vector<npa::Symbol>> error_node_symbols;
  std::vector<std::vector<npa::Symbol>> forward_node_symbols;
  unsigned entry_procedure = 0;
};

BooleanNPAProgram buildBooleanNPAProgram(
    const BooleanProgram &program, const std::string &entry = "main");

} // namespace frontend
} // namespace verification
} // namespace lotus
