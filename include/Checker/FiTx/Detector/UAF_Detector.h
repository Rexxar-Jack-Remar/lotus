#pragma once
#include <string>
#include <vector>

#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

namespace UseAfterFree {
  void defineStates(framework::StateManager& manager);
} // namespace UseAfterFree
