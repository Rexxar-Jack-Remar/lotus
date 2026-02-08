#pragma once
#include <string>
#include <vector>

#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

namespace MemoryLeak {
  void defineStates(framework::StateManager& manager);
} // namespace MemoryLeak
