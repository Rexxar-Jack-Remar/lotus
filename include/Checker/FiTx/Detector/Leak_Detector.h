#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace MemoryLeak {
void defineStates(framework::StateManager &manager);
} // namespace MemoryLeak
