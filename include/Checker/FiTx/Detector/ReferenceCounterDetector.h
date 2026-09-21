/** @file ReferenceCounterDetector.h @brief Reference count mismatch detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/RefCount.h"
#include "Checker/FiTx/Frontend/State.h"

namespace ReferenceCounter {
void defineStates(fitx::StateManager &manager);
} // namespace ReferenceCounter
