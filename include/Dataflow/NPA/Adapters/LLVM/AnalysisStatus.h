#ifndef NPA_LLVM_ANALYSIS_STATUS_H
#define NPA_LLVM_ANALYSIS_STATUS_H

#include "Dataflow/NPA/Adapters/LLVM/CallResolution.h"
#include "Dataflow/NPA/Solver/Statistics.h"

namespace npa {

struct AnalysisStatus {
  Stat summary_solve;
  double phase_artifact_construction_time = 0.0;
  double phase_summary_materialization_time = 0.0;
  double phase_propagation_time = 0.0;
  long propagation_steps = 0;
  bool propagation_converged = true;
  bool propagation_hit_limit = false;
  bool configuration_error = false;
  bool unsupported_specs = false;
  bool approximated = false;
  bool used_summary_overflow = false;
  bool used_fact_widening = false;
  bool used_bounded_inner_solve = false;
  bool overall_converged = true;
  bool overall_hit_limit = false;
  IndirectCallResolutionMode call_resolution_mode =
      IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  long indirect_calls_seen = 0;
  long unresolved_indirect_calls = 0;
  long fallback_call_edges = 0;
  bool requires_external_callee_resolver = false;
  bool open_world_unsound_mode = true;
};

} // namespace npa

#endif // NPA_LLVM_ANALYSIS_STATUS_H
