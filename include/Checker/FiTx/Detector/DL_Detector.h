/** @file DL_Detector.h @brief Double-lock detector for FiTx analysis. */
#pragma once

#include "Checker/FiTx/Detector/Lock.h"
#include "Checker/FiTx/Frontend/State.h"

namespace DoubleLock {
void define_states(fitx::StateManager &manager);
} // namespace DoubleLock
