#pragma once
#include "alloc.hpp"
#include "Checker/FiTx/frontend/State.hpp"
#include <vector>

namespace DoubleFree {
  void define_states(framework::StateManager& manager);
} // namespace DoubleFree
