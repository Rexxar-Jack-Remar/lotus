/** @file NullPtr_Detector.h @brief Null pointer dereference detector for FiTx analysis. */
#pragma once

#include "Checker/FiTx/Frontend/State.h"

namespace NullPointer {
void defineStates(fitx::StateManager &manager);
} // namespace NullPointer
