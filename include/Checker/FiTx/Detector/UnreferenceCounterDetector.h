/** @file UnreferenceCounterDetector.h @brief Unref (missing decrement) detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/RefCount.h"
#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace UnreferenceCounter {
void defineStates(fitx::StateManager &manager);
} // namespace UnreferenceCounter
