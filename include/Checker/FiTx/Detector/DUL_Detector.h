/** @file DUL_Detector.h @brief Double-unlock detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/Lock.h"
#include "Checker/FiTx/Frontend/State.h"

namespace DoubleUnlock {
void defineStates(fitx::StateManager &manager);
} // namespace DoubleUnlock
