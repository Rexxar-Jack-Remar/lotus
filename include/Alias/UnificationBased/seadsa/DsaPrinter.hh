#pragma once

#include "llvm/Pass.h"

namespace seadsa {

/// Pass to print DSA graphs for each function
llvm::Pass *createDsaPrinterPass();

} // namespace seadsa
