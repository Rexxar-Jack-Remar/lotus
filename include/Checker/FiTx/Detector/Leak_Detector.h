/** @file Leak_Detector.h @brief Memory leak detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace MemoryLeak {
void defineStates(fitx::StateManager &manager);
} // namespace MemoryLeak
