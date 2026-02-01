#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/MemorySSA/MemorySSA.h"

//===----------------------------------------------------------------------===//
/// @file IPStoreSinking.cpp
/// @brief Inter-procedural Store Sinking pass implementation
///
/// This file implements a conservative store sinking pass that moves store
/// instructions closer to their uses within a basic block while preserving
/// program semantics.
///
/// Store sinking reduces register pressure by moving stores as close as
/// possible to their first observable use, while ensuring that no side-effect
/// free instructions are moved past.
///
///===----------------------------------------------------------------------===//

namespace previrt {
namespace transforms {

using namespace llvm;
using namespace analysis;

static cl::opt<bool> OnlySingletonSink(
    "ip-sink-only-singleton",
    cl::desc("IP Store Sinking: only singleton memory regions"), cl::Hidden,
    cl::init(true));

// Conservative store sinking that keeps stores before their first observable
// use while moving them closer to that use. We only sink inside a single basic
// block and only past instructions that are side-effect free.
//
// Pseudocode:
//   for each shadow.mem.store + Store S pair in BB:
//     find earliest user U of the shadow.mem value inside BB that is after S
//     if no U: skip
//     if any instruction between S and U reads/writes memory or is a
//     terminator:
//       skip (unsafe)
//     else move S before U, move shadow.mem.store just before S

/// @brief Inter-procedural Store Sinking pass
///
/// This pass moves store instructions closer to their uses within a basic
/// block. It is conservative and only sinks stores past side-effect-free
/// instructions to maintain program semantics.
///
/// The algorithm works by:
/// 1. Finding each store instruction with its shadow.mem marker
/// 2. Locating the first use of the shadow.mem value within the same block
/// 3. Verifying all instructions between the store and the use are safe to
/// cross
/// 4. Moving the store just before its first use
class IPStoreSinking : public ModulePass {
public:
  /// @brief Unique pass identifier
  static char ID;

  /// @brief Default constructor
  IPStoreSinking() : ModulePass(ID) {}

  /// @brief Run the store sinking pass on a module
  /// @param M The LLVM module to process
  /// @return true if any stores were sunk, false otherwise
  bool runOnModule(Module &M) override {
    if (M.begin() == M.end())
      return false;

    unsigned NumSunk = 0;

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;

      for (BasicBlock &BB : F) {
        for (auto It = BB.begin(), Et = BB.end(); It != Et; ++It) {
          CallBase *CB = dyn_cast<CallBase>(&*It);
          if (!CB || !isMemSSAStore(CB, OnlySingletonSink))
            continue;

          auto NextIt = std::next(It);
          if (NextIt == BB.end())
            continue;
          StoreInst *SI = dyn_cast<StoreInst>(&*NextIt);
          if (!SI)
            continue;

          Instruction *FirstUser = nullptr;
          for (Use &U : CB->uses()) {
            if (Instruction *UI = dyn_cast<Instruction>(U.getUser())) {
              if (UI->getParent() != &BB)
                continue; // we only sink within the block
              if (UI == CB || UI == SI)
                continue;
              if (!SI->comesBefore(UI))
                continue;
              if (FirstUser == nullptr || UI->comesBefore(FirstUser)) {
                FirstUser = UI;
              }
            }
          }

          if (!FirstUser)
            continue;

          // Ensure every instruction between SI and FirstUser is side-effect
          // free so that moving the store does not change semantics.
          bool Safe = true;
          for (auto MoveIt = std::next(NextIt);
               MoveIt != BB.end() && &*MoveIt != FirstUser; ++MoveIt) {
            if (MoveIt->mayReadOrWriteMemory() || MoveIt->isTerminator()) {
              Safe = false;
              break;
            }
          }

          if (!Safe)
            continue;

          SI->moveBefore(FirstUser);
          CB->moveBefore(SI);
          NumSunk++;
        }
      }
    }

    if (NumSunk > 0) {
      errs() << "IP-Sink: sunk " << NumSunk << " stores\n";
    }
    return NumSunk > 0;
  }

  /// @brief Specify analysis dependencies and preserves
  /// @param AU Analysis usage information to populate
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }

  /// @brief Get the name of this pass
  /// @return The pass name as a string reference
  StringRef getPassName() const override {
    return "Interprocedural Store Sinking";
  }
};

char IPStoreSinking::ID = 0;

static RegisterPass<IPStoreSinking> X("ip-sink",
                                      "Interprocedural Store Sinking");

} // namespace transforms
} // namespace previrt
