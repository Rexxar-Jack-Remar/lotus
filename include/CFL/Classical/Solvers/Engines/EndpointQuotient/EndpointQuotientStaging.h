#pragma once

#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <vector>

namespace lotus::cfl::endpoint {

/// Conservative classification of a grammar dependency SCC. Specialized
/// evaluators may handle the regular and transitive stages; General stages
/// must use the complete CFL saturation algorithm.
enum class SccKind {
  /// A singleton symbol with no recursive dependency.
  Acyclic,
  /// Every recursive dependency is introduced by a unary production.
  UnaryRegular,
  /// A singleton whose recursive binary productions are all A -> A X, where
  /// X belongs to a strict dependency stage. Unary A -> A is also permitted.
  LeftLinear,
  /// A singleton whose recursive binary productions are all A -> X A, where
  /// X belongs to a strict dependency stage. Unary A -> A is also permitted.
  RightLinear,
  /// A singleton whose only non-unary recursion is A -> A A.
  TransitiveSelf,
  /// Mutually recursive or nonlinear productions not covered above.
  General,
};

struct StagingStage {
  /// Dense index in dependency-first topological order.
  Id index = 0;
  SccKind kind = SccKind::Acyclic;

  /// Sorted symbols in this strongly connected component.
  std::vector<Id> symbols;

  /// Canonical, duplicate-free productions whose LHS belongs to this stage.
  std::vector<Rule> rules;

  /// Subset of rules that mention a symbol from this stage on their RHS.
  std::vector<Rule> recursive_rules;

  /// Strict predecessor/successor stage indices, sorted in plan order.
  std::vector<Id> dependencies;
  std::vector<Id> dependents;
};

struct StagingPlan {
  /// Stages are ordered so every strict dependency precedes its dependent.
  /// Ties are resolved by the smallest symbol ID in a component.
  std::vector<StagingStage> stages;

  /// Maps every symbol in the problem to its stage index.
  std::vector<Id> symbol_to_stage;

  const StagingStage &stageForSymbol(Id symbol) const;
};

/// Builds a deterministic grammar-only staging plan. Input edges do not affect
/// the plan. The problem is validated before its productions are inspected.
StagingPlan buildStagingPlan(const Problem &problem);

} // namespace lotus::cfl::endpoint
