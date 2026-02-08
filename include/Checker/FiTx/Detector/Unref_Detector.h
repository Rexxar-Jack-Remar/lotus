#pragma once
#include <string>
#include <vector>

#include "Checker/FiTx/Detector/Ref_count.h"
#include "Checker/FiTx/Frontend/State.h"

namespace UnreferenceCounter {
  void defineStates(framework::StateManager& manager);
} // namespace UnreferenceCounter
