#pragma once

#include "Dataflow/APA/Core/Options.h"

namespace elimination::order {

struct StructuralPolicy final {
  static double score(const OrderSignals &Signals) {
    return Signals.structural;
  }
};

} // namespace elimination::order
