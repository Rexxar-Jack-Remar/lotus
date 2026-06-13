// Module optimizer that runs LLVM's standard optimization pipelines.
// This provides a simple interface for applying O0, O1, O2, or O3
// optimizations.
//
//===----------------------------------------------------------------------===//
/// @file ModuleOptimizer.cpp
/// @brief Module optimization pipeline implementation
///
/// This file implements utility functions for running standard LLVM
/// optimization pass pipelines on entire modules. It leverages LLVM's
/// PassBuilder infrastructure to construct and execute optimization pipelines
/// at various levels (O0-O3).
///
/// The module optimizer supports:
/// - Different optimization levels (O0, O1, O2, O3)
/// - Automatic registration of required analysis managers
/// - Cross-module pass coordination via proxy registrations
///
/// @par Usage Example:
/// @code
/// #include "Optimization/PassOrdering/ModuleOptimizer.h"
/// using namespace llvm_utils;
///
/// optimiseModule(M, OptimizationLevel::O3);
/// @endcode
///
///===----------------------------------------------------------------------===//

#include "Optimization/PassOrdering/ModuleOptimizer.h"

#include <stdexcept>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

namespace llvm_utils {

using namespace llvm;

/// @brief Run the default O0, O1, O2, or O3 optimization pass pipelines on the
/// given module.
///
/// This function applies LLVM's standard optimization pipeline to transform the
/// input module. The PassBuilder constructs an appropriate sequence of passes
/// based on the specified optimization level.
///
/// @param M Pointer to the LLVM module to optimize (must not be null)
/// @param OptLevel The optimization level to apply:
///   - OptimizationLevel::O0: No optimizations (useful for debugging)
///   - OptimizationLevel::O1: Basic optimizations
///   - OptimizationLevel::O2: Standard optimizations (default for release
///   builds)
///   - OptimizationLevel::O3: Aggressive optimizations (may increase code size)
///
/// @return PreservedAnalyses containing the set of analyses that were preserved
///         after running the optimization pipeline
///
/// @throws std::invalid_argument if M is a null pointer
auto optimiseModule(Module *M, OptimizationLevel OptLevel)
    -> PreservedAnalyses {
  if (!M)
    throw std::invalid_argument("Null ptr argument!");

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  return PB.buildPerModuleDefaultPipeline(OptLevel).run(*M, MAM);
}

} // namespace llvm_utils
