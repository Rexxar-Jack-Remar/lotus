/** @file DF_Detector.h @brief Double-free detector for FiTx analysis. */
#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

#include <vector>

namespace DoubleFree {
void define_states(fitx::StateManager &manager);
} // namespace DoubleFree
