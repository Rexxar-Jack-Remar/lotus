#pragma once
#include <string>
#include <vector>

#include "refcount.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace UnreferenceCounter {
  void defineStates(framework::StateManager& manager);
} // namespace UnreferenceCounter
