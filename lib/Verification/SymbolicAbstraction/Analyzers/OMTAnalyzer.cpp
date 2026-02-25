/*

 * Author: rainoftime
*/
#include "Verification/SymbolicAbstraction/Analyzers/Analyzer.h"
#include "Verification/SymbolicAbstraction/Core/ConcreteState.h"
#include "Verification/SymbolicAbstraction/Core/ValueMapping.h"
#include "Verification/SymbolicAbstraction/Utils/Config.h"
#include "Verification/SymbolicAbstraction/Utils/Utils.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace symbolic_abstraction {
namespace {
void addObjectiveIfNew(const z3::expr &objective,
                       std::vector<z3::expr> *objectives,
                       std::set<std::string> *seen) {
  std::string key = objective.to_string();
  if (seen->insert(key).second)
    objectives->push_back(objective);
}

/**
 * @brief Collect OMT objectives from represented bitvector variables.
 *
 * We use a generic template bank so OMT can refine more than interval-only
 * domains. For each represented bitvector value x, we add:
 * - x (bitvector objective)
 * - bv2int(x, unsigned)
 * - bv2int(x, signed)
 *
 * Optionally, we also add signed pairwise differences (x - y) for values with
 * equal bitwidth to support relational domains.
 */
void collectObjectives(const FunctionContext &fctx, const ValueMapping &vmap,
                       std::vector<z3::expr> *objectives,
                       unsigned max_objectives, bool enable_pair_objectives,
                       unsigned max_bit_objectives_per_var) {
  std::set<std::string> seen;
  std::vector<z3::expr> vars;
  vars.reserve(fctx.representedValues().size());

  for (llvm::Value *value : fctx.representedValues()) {
    z3::sort sort = fctx.sortForType(value->getType());
    if (!sort.is_bv())
      continue;

    z3::expr x = vmap[value];
    vars.push_back(x);

    addObjectiveIfNew(x, objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    addObjectiveIfNew(z3::bv2int(x, false), objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    addObjectiveIfNew(z3::bv2int(x, true), objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    // Bit-level objectives help bitmask/congruence-like domains by forcing
    // models that witness variability/fixity of individual bits.
    unsigned bw = x.get_sort().bv_size();
    unsigned bits_to_add = std::min(max_bit_objectives_per_var, bw);
    for (unsigned bit = 0; bit < bits_to_add; ++bit) {
      addObjectiveIfNew(z3::bv2int(x.extract(bit, bit), false), objectives,
                        &seen);
      if (objectives->size() >= max_objectives)
        return;
    }
  }

  if (!enable_pair_objectives)
    return;

  for (unsigned i = 0; i < vars.size(); ++i) {
    for (unsigned j = i + 1; j < vars.size(); ++j) {
      if (vars[i].get_sort().bv_size() != vars[j].get_sort().bv_size())
        continue;

      z3::expr diff = z3::bv2int(vars[i], true) - z3::bv2int(vars[j], true);
      addObjectiveIfNew(diff, objectives, &seen);
      if (objectives->size() >= max_objectives)
        return;

      // Octagon/zone-style template.
      z3::expr sum = z3::bv2int(vars[i], true) + z3::bv2int(vars[j], true);
      addObjectiveIfNew(sum, objectives, &seen);
      if (objectives->size() >= max_objectives)
        return;
    }
  }
}
} // namespace

/**
 * @brief Run an optimization query to find the maximum or minimum value of an objective.
 *
 * Uses Z3's optimize solver with "box" priority to find an optimal solution for
 * the given objective expression subject to the constraint formula phi. Updates
 * the target abstract value with the concrete state from the optimal model if
 * satisfiable.
 *
 * @param objective The Z3 expression to optimize (typically a variable from vmap)
 * @param phi The constraint formula that must be satisfied
 * @param vmap Value mapping for converting models to concrete states
 * @param[out] target Abstract value to update with the optimal concrete state
 * @param maximize If true, maximize the objective; otherwise minimize
 * @param timeout_ms Timeout in milliseconds (0 means no timeout)
 * @return OptimizeStatus indicating whether the optimization succeeded, failed, or timed out
 */
OMTAnalyzer::OptimizeStatus
OMTAnalyzer::runOptimize(const z3::expr &objective, const z3::expr &phi,
                         const ValueMapping &vmap, AbstractValue *target,
                         bool maximize, unsigned timeout_ms) const {
  z3::optimize opt(phi.ctx());
  opt.add(phi);

  z3::params params(phi.ctx());
  params.set("priority", "box");
  if (timeout_ms > 0)
    params.set("timeout", timeout_ms);
  opt.set(params);

  if (maximize)
    opt.maximize(objective);
  else
    opt.minimize(objective);

  // Keep SMT call accounting comparable with other analyzers.
  countSmtSolverCall();
  auto res = opt.check();
  if (res == z3::sat) {
    ConcreteState cstate(vmap, opt.get_model());
    target->updateWith(cstate);
    return OptimizeStatus::Sat;
  }

  if (res == z3::unsat)
    return OptimizeStatus::Unsat;

  return OptimizeStatus::Unknown;
}

bool OMTAnalyzer::overapproximateToTop(AbstractValue *result) const {
  auto top = std::unique_ptr<AbstractValue>(result->clone());
  top->havoc();
  return result->joinWith(*top);
}

/**
 * @brief Compute the strongest abstract consequence using OMT optimization.
 *
 * This is the main entry point for computing strongest consequences in OMTAnalyzer.
 * The algorithm:
 * 1. Checks feasibility of phi
 * 2. If infeasible, leaves result unchanged
 * 3. Collects generic objectives from represented bitvector variables
 * 4. For each objective, optimizes both max and min to collect extremal models
 * 5. Joins all optimal solutions into the result
 * 6. If OMT cannot conclude (unknown/incomplete), conservatively overapproximates to top
 *
 * @param[in,out] result The abstract value to refine (updated in place)
 * @param phi The constraint formula representing the concrete semantics
 * @param vmap Value mapping for converting models to concrete states
 * @return true if the abstract value was changed, false otherwise
 */
bool OMTAnalyzer::strongestConsequence(AbstractValue *result, z3::expr phi,
                                       const ValueMapping &vmap) const {
  z3::context &ctx = phi.ctx();
  auto cfg = FunctionContext_.getConfig();
  unsigned timeout_ms = cfg.get<int>("Analyzer", "OMTTimeoutMs", 10000);
  int max_objectives_cfg = cfg.get<int>("Analyzer", "OMTMaxObjectives", 512);
  unsigned max_objectives =
      max_objectives_cfg > 0 ? (unsigned)max_objectives_cfg : 512u;
  bool pair_objectives =
      cfg.get<bool>("Analyzer", "OMTPairObjectives", true);
  int max_bits_cfg =
      cfg.get<int>("Analyzer", "OMTMaxBitObjectivesPerVar", 8);
  unsigned max_bit_objectives_per_var =
      max_bits_cfg > 0 ? (unsigned)max_bits_cfg : 8u;
  bool retry_unknown_without_timeout =
      cfg.get<bool>("Analyzer", "OMTRetryUnknownWithoutTimeout", true);

  z3::solver feasibility(ctx);
  feasibility.add(phi);
  auto feas_res = checkWithStats(&feasibility);

  if (feas_res == z3::unsat) {
    // Match Unilateral/Bilateral behavior in Analyzer::bestTransformer:
    // joining with alpha(false) should leave result unchanged.
    return false;
  }

  if (feas_res == z3::unknown)
    return overapproximateToTop(result);

  std::vector<z3::expr> objectives;
  collectObjectives(FunctionContext_, vmap, &objectives, max_objectives,
                    pair_objectives, max_bit_objectives_per_var);
  if (objectives.empty())
    return overapproximateToTop(result);

  auto candidate = std::unique_ptr<AbstractValue>(result->clone());
  candidate->resetToBottom();

  bool saw_unknown = false;
  for (auto &obj : objectives) {
    auto run_with_retry = [&](bool maximize) {
      OptimizeStatus res =
          runOptimize(obj, phi, vmap, candidate.get(), maximize, timeout_ms);
      if (res == OptimizeStatus::Unknown && retry_unknown_without_timeout &&
          timeout_ms > 0) {
        res = runOptimize(obj, phi, vmap, candidate.get(), maximize, 0);
      }
      return res;
    };

    auto max_res = run_with_retry(true);
    auto min_res = run_with_retry(false);

    if (max_res == OptimizeStatus::Unsat || min_res == OptimizeStatus::Unsat) {
      // phi satisfiable was checked above; objective optimization being UNSAT
      // indicates an inconsistent optimize query state. Use a conservative
      // overapproximation to preserve soundness.
      return overapproximateToTop(result);
    }

    if (max_res == OptimizeStatus::Unknown ||
        min_res == OptimizeStatus::Unknown) {
      saw_unknown = true;
    }
  }

  if (saw_unknown)
    return overapproximateToTop(result);

  return result->joinWith(*candidate);
}

} // namespace symbolic_abstraction
