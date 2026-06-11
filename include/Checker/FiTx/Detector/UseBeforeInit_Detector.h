/** @file UseBeforeInit_Detector.h @brief Use-before-initialization detector for FiTx analysis. */
#pragma once

#include "Checker/FiTx/Frontend/State.h"

namespace UseBeforeInitialization {
void defineStates(fitx::StateManager &manager);
} // namespace UseBeforeInitialization
