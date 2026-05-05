#pragma once

/**
 * @file MonoTest.cpp
 * @brief Unit tests for Mono (monotone dataflow framework)
 */

#include "Dataflow/Mono/Analyses/Inter/InterTaintAnalysis.h"
#include "Dataflow/Mono/Analyses/Inter/InterConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Inter/InterFullConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraFullConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"
#include "Dataflow/Mono/Core/CallStringSolver.h"
#include "Dataflow/Mono/Solver/InterSolver.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
#include "Dataflow/Mono/Support/Result.h"
#include "TestUtils/LLVMHelpers.h"

#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mono;

class MonoTest : public lotus::unittest::LlvmModuleTest {
protected:
  template <typename InstT> InstT *findFirst(Function *F) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *Match = dyn_cast<InstT>(&I)) {
          return Match;
        }
      }
    }
    return nullptr;
  }

  Instruction *findByOpcode(Function *F, unsigned Opcode) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (I.getOpcode() == Opcode) {
          return &I;
        }
      }
    }
    return nullptr;
  }
};

// Test live variables analysis on simple function

// Test live variables with multiple blocks

// Test empty function






























