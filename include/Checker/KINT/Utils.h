/** @file Utils.h @brief Utility functions for KINT integer analysis. */
#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace kint {

// Get unique identifier for LLVM instruction
int64_t get_id(llvm::Instruction *Inst);

} // namespace kint
