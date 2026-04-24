/**
 * @file RunPrepass.cpp
 * @brief Orchestrates all preprocessing passes required before TPA analysis.
 *
 * This module runs a sequence of LLVM transformation passes that prepare the IR
 * for TPA). The passes include global cleanup,
 * alias resolution, global constructor lowering, and various expansion passes
 * that simplify the IR structure.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"

#include "Alias/InclusionBased/TPA/Transforms/ExpandByVal.h"
#include "Alias/InclusionBased/TPA/Transforms/ExpandConstantExpr.h"
#include "Alias/InclusionBased/TPA/Transforms/ExpandGetElementPtr.h"
#include "Alias/InclusionBased/TPA/Transforms/ExpandIndirectBr.h"
#include "Alias/InclusionBased/TPA/Transforms/FoldIntToPtr.h"
#include "Alias/InclusionBased/TPA/Transforms/GlobalCleanup.h"
#include "Alias/InclusionBased/TPA/Transforms/LowerGlobalCtor.h"

#include <llvm/IR/Module.h>

using namespace llvm;

namespace transform {

/**
 * @brief Run all preprocessing passes in the correct order.
 *
 * Executes module-level passes first (GlobalCleanup, ResolveAliases,
 * LowerGlobalCtor, ExpandIndirectBr, ExpandByVal), then function-level passes
 * (ExpandConstantExpr, FoldIntToPtr, ExpandGetElementPtr) on each function.
 *
 * @param module The LLVM module to transform
 */
static void runAllPrepasses(Module &module) {
  ModuleAnalysisManager MAM;
  FunctionAnalysisManager FAM;

  // Run Module-level passes
  GlobalCleanup cleanup;
  cleanup.run(module, MAM);

  ResolveAliases resolver;
  resolver.run(module, MAM);

  LowerGlobalCtorPass lowerCtor;
  lowerCtor.run(module, MAM);

  ExpandIndirectBr expIndirectBr;
  expIndirectBr.run(module, MAM);

  ExpandByValPass expByVal;
  expByVal.run(module, MAM);

  // Run Function-level passes
  ExpandConstantExprPass expConstExpr;
  FoldIntToPtrPass foldIntToPtr;
  ExpandGetElementPtrPass expGEP;

  for (auto &F : module) {
    expConstExpr.run(F, FAM);
    foldIntToPtr.run(F, FAM);
    expGEP.run(F, FAM);
  }
}

/**
 * @brief Public entry point to run all preprocessing passes on a module.
 *
 * @param module The LLVM module to preprocess for TPA analysis
 */
void runPrepassOn(Module &module) { runAllPrepasses(module); }

} // namespace transform
