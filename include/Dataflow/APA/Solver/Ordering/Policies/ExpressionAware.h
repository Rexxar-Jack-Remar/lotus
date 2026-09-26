#pragma once

#include "Dataflow/APA/Core/Options.h"

namespace elimination::order {

struct ExpressionAwarePolicy final {
  // Always use the estimated bypass growth, never silently substitute degree.
  static double score(const OrderSignals &Signals) {
    return Signals.expression;
  }
};

} // namespace elimination::order
