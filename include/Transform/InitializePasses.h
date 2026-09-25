//
// This file is distributed under the MIT License. See LICENSE for details.
//
#pragma once

#include "llvm/InitializePasses.h"

namespace llvm {
void initializeDevirtualizePass(PassRegistry &Registry);
} // end namespace llvm

