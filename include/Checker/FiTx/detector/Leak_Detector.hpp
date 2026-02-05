#pragma once
#include <string>
#include <vector>

#include "alloc.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace MemoryLeak {
  void defineStates(framework::StateManager& manager);
} // namespace MemoryLeak
