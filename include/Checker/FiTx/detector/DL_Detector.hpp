#pragma once

#include "lock.hpp"
#include "Checker/FiTx/frontend/State.hpp"

namespace DoubleLock {
  void define_states(framework::StateManager& manager);
} // namespace DoubleLock
