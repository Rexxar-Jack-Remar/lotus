#pragma once

#include <cstdint>

#include <llvm/ADT/StringRef.h>

namespace lotus {

using UniqueIRID = uint64_t;

class UniqueIRConstants {
public:
  static const llvm::StringRef InstructionID;
  static const llvm::StringRef BasicBlockID;
  static const llvm::StringRef LoopID;
  static const llvm::StringRef FunctionID;
  static const llvm::StringRef ModuleID;
};

} // namespace lotus
