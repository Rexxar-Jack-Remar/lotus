#pragma once
#include <string>
#include <vector>

#include "alloc.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace UseAfterFree {
  void defineStates(framework::StateManager& manager);
} // namespace UseAfterFree
