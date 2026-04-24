#pragma once

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace transform {

// These functions are defined in ExpandUtils.cpp
extern Instruction *phiSafeInsertPt(Use *use);
extern void phiSafeReplaceUses(Use *use, Value *newVal);

} // namespace transform
