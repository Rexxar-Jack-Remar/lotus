#ifndef NPA_SOLVER_STATISTICS_H
#define NPA_SOLVER_STATISTICS_H

#include "Dataflow/NPA/Solver/Options.h"

#include <vector>

namespace npa {

struct NewtonRoundStat {
  bool used_tensor = false;
  bool tensor_fallback = false;
  int active_coordinates = 0;
  long queried_occurrences = 0;
  long retained_occurrences = 0;
  long materialized_derivative_terms = 0;
  double discovery_time = 0.0;
  double materialization_time = 0.0;
  double linear_solve_time = 0.0;
};

struct Stat {
  double time{};
  double equation_validation_time = 0.0;
  double newton_initialization_time = 0.0;
  int iters{};
  bool converged = true;
  bool hit_limit = false;
  bool hit_outer_limit = false;
  bool hit_linear_limit = false;
  bool hit_fixpoint_limit = false;
  int equation_count = 0;
  int requested_max_iters = -1;
  int effective_max_iters = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  NewtonRoundStrategy newton_round_strategy = NewtonRoundStrategy::Dense;
  ConvergencePolicy convergence_policy = ConvergencePolicy::DomainDefault;
  bool used_approx_equal = false;
  bool used_auto_n_cap = false;
  bool retried_without_auto_n_cap = false;
  bool adaptive_scc_used = false;
  int adaptive_scc_direct_count = 0;
  int adaptive_scc_worklist_count = 0;
  int adaptive_scc_tensor_count = 0;
  int adaptive_scc_tensor_fallback_count = 0;
  int tensor_rounds = 0;
  int tensor_fallback_rounds = 0;
  bool domain_contract_checks_run = false;
  bool domain_contract_checks_failed = false;
  long indexed_derivative_occurrences = 0;
  long queried_derivative_occurrences = 0;
  long retained_derivative_occurrences = 0;
  long materialized_derivative_terms = 0;
  long active_coordinate_visits = 0;
  double occurrence_index_time = 0.0;
  double round_discovery_time = 0.0;
  double round_materialization_time = 0.0;
  double linear_solve_time = 0.0;
  std::vector<NewtonRoundStat> newton_rounds;
};

} // namespace npa

#endif // NPA_SOLVER_STATISTICS_H
