/**
 * @file ICFGTest.cpp
 * @brief Comprehensive unit tests for Interprocedural Control Flow Graph (ICFG)
 * 
 * ICFG represents the interprocedural control flow of a program,
 * connecting call sites to function entry/exit points.
 */

#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/GraphAnalysis.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

#include <vector>

using namespace llvm;
using namespace lotus::unittest;

class ICFGTest : public LlvmModuleTest {
};

// Test 1: Simple function with entry and exit blocks
#include "Fragments/ICFGConstruction.inc"
#include "Fragments/ICFGCallsAndExceptions.inc"
#include "Fragments/ICFGMutationAndLifecycle.inc"
