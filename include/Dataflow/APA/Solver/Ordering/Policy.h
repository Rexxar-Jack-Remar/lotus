#pragma once

#include "Dataflow/APA/Solver/Ordering/Policies/Baselines.h"
#include "Dataflow/APA/Solver/Ordering/Policies/ExpressionAware.h"
#include "Dataflow/APA/Solver/Ordering/Policies/Hybrid.h"
#include "Dataflow/APA/Solver/Ordering/Policies/StarRisk.h"
#include "Dataflow/APA/Solver/Ordering/Policies/Structural.h"

#include <cmath>

namespace elimination::order {

struct PolicyRequirements final {
  bool expression_growth = false;
  bool star_exposure = false;
  bool new_fill = false;
};

inline PolicyRequirements requirements(OrderingPolicy Policy) {
  switch (Policy) {
  case OrderingPolicy::ExpressionAware:
    return {true, false, false};
  case OrderingPolicy::StarRisk:
    return {false, true, false};
  case OrderingPolicy::Hybrid:
    return {true, true, false};
  case OrderingPolicy::MinFill:
    return {false, false, true};
  case OrderingPolicy::Default:
  case OrderingPolicy::CostAware:
  case OrderingPolicy::Structural:
  case OrderingPolicy::ReversePostOrder:
  case OrderingPolicy::Random:
  case OrderingPolicy::MinDegree:
  case OrderingPolicy::Explicit:
    return {};
  }
  throw std::invalid_argument("unknown APA ordering policy");
}

inline void validateOrderOptions(const OrderPolicyOptions &Options,
                                 OrderingPolicy Policy, std::size_t N) {
  requirements(Policy); // Reject unknown policies; never select a substitute.
  for (double Cap :
       {Options.StructuralCap, Options.ExpressionCap, Options.StarCap}) {
    if (!std::isfinite(Cap) || Cap <= 0.0) {
      throw std::invalid_argument(
          "APA ordering normalization caps must be finite and positive");
    }
  }
  if (Policy == OrderingPolicy::Explicit)
    ExplicitPolicy::validate(Options.ExplicitOrder, N);
}

// Registry/dispatch only. Each core policy owns its score in a named header.
// Rank, degree and fixed priority belong to baseline selection, not cost
// signals.
inline double policyScore(OrderingPolicy Policy, const OrderSignals &Signals,
                          const OrderPolicyOptions &Options,
                          std::size_t Degree = 0, std::size_t Rank = 0,
                          std::size_t Priority = 0) {
  switch (Policy) {
  case OrderingPolicy::Default:
  case OrderingPolicy::CostAware:
  case OrderingPolicy::Structural:
    return StructuralPolicy::score(Signals);
  case OrderingPolicy::ExpressionAware:
    return ExpressionAwarePolicy::score(Signals);
  case OrderingPolicy::StarRisk:
    return StarRiskPolicy::score(Signals, Options);
  case OrderingPolicy::Hybrid:
    return HybridPolicy::score(Signals, Options);
  case OrderingPolicy::MinDegree:
    return MinDegreePolicy::score(Degree);
  case OrderingPolicy::MinFill:
    return MinFillPolicy::score(Signals.fill);
  case OrderingPolicy::ReversePostOrder:
    return ReversePostOrderPolicy::score(Rank);
  case OrderingPolicy::Random:
    return RandomPolicy::score(Priority);
  case OrderingPolicy::Explicit:
    return ExplicitPolicy::score(Priority);
  }
  throw std::invalid_argument("unknown APA ordering policy");
}

} // namespace elimination::order
