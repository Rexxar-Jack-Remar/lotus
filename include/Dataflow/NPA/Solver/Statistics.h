#ifndef NPA_SOLVER_STATISTICS_H
#define NPA_SOLVER_STATISTICS_H

#include "Dataflow/NPA/Solver/Options.h"

namespace npa {

struct Stat {
  double time{};
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
  bool used_approx_equal = false;
  bool used_auto_n_cap = false;
  bool retried_without_auto_n_cap = false;
  bool adaptive_scc_used = false;
  int adaptive_scc_direct_count = 0;
  int adaptive_scc_worklist_count = 0;
  int adaptive_scc_tensor_count = 0;
  int adaptive_scc_tensor_fallback_count = 0;
  bool domain_contract_checks_run = false;
  bool domain_contract_checks_failed = false;
};

} // namespace npa

#endif // NPA_SOLVER_STATISTICS_H
