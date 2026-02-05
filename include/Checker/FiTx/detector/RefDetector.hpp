#pragma once
#include "refcount.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace ReferenceCounter {
  void defineStates(framework::StateManager& manager);
} // namespace ReferenceCounter
