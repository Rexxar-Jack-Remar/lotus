#ifndef NPA_SOLVER_OPTIONS_H
#define NPA_SOLVER_OPTIONS_H

namespace npa {

/// Backend for the linearized equation system inside a Newton round.
enum class LinearStrategy {
  Naive,
  SCC,
  AdaptiveScc,
  TensorProduct,
};

enum class DomainContractMode {
  Off,
  BasicChecks,
};

struct SolveOptions {
  bool verbose = false;
  int max_iterations = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  DomainContractMode contract_mode = DomainContractMode::Off;
};

} // namespace npa

#endif // NPA_SOLVER_OPTIONS_H
