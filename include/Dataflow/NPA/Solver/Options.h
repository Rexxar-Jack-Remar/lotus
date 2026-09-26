#pragma once

#include "Dataflow/NPA/Core/Domain.h"

namespace npa {

/// Backend for the linearized equation system inside a Newton round.
enum class LinearStrategy {
  Naive,
  SCC,
  AdaptiveScc,
  TensorProduct,
};

/// Construction policy for an idempotent Newton round.
///
/// Dense preserves the traditional residual-based implementation.  The other
/// policies use the fixed seed F(0) and solve only its reachable derivative
/// subsystem.  Static computes syntactic reachability once, AlwaysMaybe repeats
/// the indexed demand traversal without pruning, and Sparse enables the
/// domain-parametric zero oracle.
enum class NewtonRoundStrategy {
  Dense,
  Static,
  AlwaysMaybe,
  Sparse,
};

enum class DomainContractMode {
  Off,
  BasicChecks,
  Strict,
};

struct SolveOptions {
  bool verbose = false;
  int max_iterations = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  NewtonRoundStrategy newton_round_strategy = NewtonRoundStrategy::Dense;
  DomainContractMode contract_mode = DomainContractMode::Off;
  ConvergencePolicy convergence_policy = ConvergencePolicy::DomainDefault;
};

} // namespace npa

