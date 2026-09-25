#pragma once

#include "Verification/Frontend/BooleanProgram.h"

namespace lotus {
namespace verification {
namespace frontend {

LoweringResult lowerToPredicateProgram(const BooleanProgram &program,
                                       const std::string &procedure = "main");

LoweredBooleanProgram lowerBooleanProgram(const BooleanProgram &program);

npa::PredicateFormula lowerExprToPredicateFormula(
    const BooleanExpr &expr,
    const std::unordered_map<std::string, unsigned> &predicate_to_index,
    npa::PredicateVariableVersion version =
        npa::PredicateVariableVersion::Current);

npa::PredicateUpdate lowerExprToPredicateUpdate(
    const BooleanExpr &expr,
    const std::unordered_map<std::string, unsigned> &predicate_to_index,
    unsigned target);

} // namespace frontend
} // namespace verification
} // namespace lotus
