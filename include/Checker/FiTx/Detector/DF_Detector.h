#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"
#include <vector>

namespace DoubleFree {
  void define_states(framework::StateManager& manager);
} // namespace DoubleFree
