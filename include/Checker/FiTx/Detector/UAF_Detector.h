#pragma once
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace UseAfterFree {
void defineStates(framework::StateManager &manager);
} // namespace UseAfterFree
