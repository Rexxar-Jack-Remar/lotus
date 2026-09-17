#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Solver/Ordering/Policies/Normalization.h"

namespace elimination::order {

struct HybridPolicy final {
  static double score(const OrderSignals &Signals,
                      const OrderPolicyOptions &Options) {
    return normalize(Signals.structural, Options.StructuralCap) +
           normalize(Signals.expression, Options.ExpressionCap) +
           normalize(Signals.star, Options.StarCap);
  }
};

} // namespace elimination::order
