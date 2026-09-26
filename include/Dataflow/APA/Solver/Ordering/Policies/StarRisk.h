#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Solver/Ordering/Policies/Normalization.h"

namespace elimination::order {

struct StarRiskPolicy final {
  static double score(const OrderSignals &Signals,
                      const OrderPolicyOptions &Options) {
    return normalize(Signals.structural, Options.StructuralCap) +
           normalize(Signals.star, Options.StarCap);
  }
};

} // namespace elimination::order
