//===-- SimplifyExtractValue.cpp - Remove extraneous extractvalue insts----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Simplify extractvalue
//
// Derived from InstCombine
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
//
// Class: SimplifyEV
//
class SimplifyEV : public PassInfoMixin<SimplifyEV> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
} // namespace llvm
