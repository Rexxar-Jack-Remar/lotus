/** @file UAF_Detector.h @brief Use-after-free detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace UseAfterFree {
void defineStates(fitx::StateManager &manager);
} // namespace UseAfterFree
