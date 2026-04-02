//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// @file AggressiveInliner.cpp
/// @brief Aggressive inliner pass implementation
///
/// This file implements an aggressive function inliner that attempts to inline
/// as many call sites as possible within a module. Unlike selective inlining
/// passes that balance compile time with performance, this pass prioritizes
/// maximum inlining to enable subsequent optimizations.
///
/// The pass processes each function in the module and inlines all direct
/// function calls that are not explicitly excluded via the command-line option.
///
///===----------------------------------------------------------------------===//

#include "Optimization/Scalar/AggressiveInliner.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <string>
#include <vector>

llvm::cl::list<std::string> noinline(
    "ainline-noinline",
    llvm::cl::desc("Do not inline the given functions (comma-separated)\n"),
    llvm::cl::CommaSeparated);

using namespace llvm;

/// @brief Aggressive inliner pass that inlines all possible call sites
///
/// This pass implements an aggressive inlining strategy that attempts to inline
/// every direct function call within a module. It is designed to maximize the
/// amount of inlining to enable subsequent optimization passes to have more
/// context and opportunities for improvement.
///
/// @note This inliner is intentionally naive and may significantly increase
/// compile time for large codebases. It is best used when maximum optimization
/// potential is desired at the cost of compilation speed.
///
/// @par Example usage:
/// @code
/// opt -ainline input.bc -o output.bc
/// opt -ainline-noinline="foo,bar" input.bc -o output.bc
/// @endcode
class AggressiveInliner : public ModulePass {
public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Default constructor
  AggressiveInliner() : ModulePass(ID) {}

  /// @brief Run the inliner on the entire module
  /// @param M The LLVM module to process
  /// @return true if any inlining was performed, false otherwise
  bool runOnModule(Module &M) override;

  /// @brief Process a single function, inlining all its call sites
  /// @param F The function to process
  /// @return true if any inlining was performed, false otherwise
  bool runOnFunction(Function &F);
};

static RegisterPass<AggressiveInliner>
    DLTU("ainline", "Aggressive inliner - inline as much as you can.");

char AggressiveInliner::ID;

ModulePass *llvm::createAggressiveInlinerPass() {
  return new AggressiveInliner();
}

/// @brief Check if a function should be skipped during inlining
/// @param name The name of the function to check
/// @return true if the function is in the noinline exclusion list, false
/// otherwise
static bool shouldIgnore(const std::string &name) {
  for (const auto &ignore : noinline) {
    if (name == ignore)
      return true;
  }
  return false;
}

bool AggressiveInliner::runOnModule(Module &M) {
  bool changed = false;
  for (auto &F : M) {
    changed |= runOnFunction(F);
  }
  return changed;
}

bool AggressiveInliner::runOnFunction(Function &F) {
  bool changed = false;
  std::vector<CallInst *> calls;
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        calls.push_back(CI);
      }
    }
  }

  // FIXME: this is really stupid naive way to inline...
  for (auto *CI : calls) {
    // llvm::errs() << "Inlining: " <<*CI << "\n";
    auto *CV = CI->getCalledOperand()->stripPointerCasts();
    auto *fun = llvm::dyn_cast<llvm::Function>(CV);
    if (!fun)
      continue; // funptr

    auto name = fun->getName().str();
    if (shouldIgnore(name))
      continue;

    InlineFunctionInfo IFI;
#if LLVM_VERSION_MAJOR > 10
    auto result = InlineFunction(*CI, IFI);
    if (!result.isSuccess()) {
      // llvm::errs() << "Failed inlining: " << *CI << "\n";
      // llvm::errs() << "  " << static_cast<const char *>(result) << "\n";
    } else {
      changed = true;
    }
#else
    auto result = InlineFunction(CI, IFI);
    if (!result) {
      // llvm::errs() << "Failed inlining: " << *CI << "\n";
      // llvm::errs() << "  " << static_cast<const char *>(result) << "\n";
    } else {
      changed = true;
    }
#endif
  }

  return changed;
}
