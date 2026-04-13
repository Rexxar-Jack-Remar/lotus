//===-- MergeGEP.cpp - Merge GEPs for indexing in arrays ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Merge chained GEPs; Specially useful for arrays inside structs
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
//
// Class: MergeArrayGEP
//
class MergeArrayGEP : public PassInfoMixin<MergeArrayGEP> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
} // namespace llvm
