/** @file API.h @brief API utilities for null-pointer analysis support. */
#pragma once

#include <set>

#include <llvm/IR/Instruction.h>

using namespace llvm;

class API {
public:
  static bool isMemoryAllocate(Instruction *);

  static bool isHeapAllocate(Instruction *);

  static bool isStackAllocate(Instruction *);

  static std::set<std::string> HeapAllocFunctions;
};

