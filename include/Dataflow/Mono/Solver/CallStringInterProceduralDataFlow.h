/**
 * A lightweight call-string based inter-procedural monotone data-flow engine.
 *
 * The implementation keeps a separate IN/OUT lattice per (Instruction, CallString)
 * pair where the call string is bounded to length K. Call strings are represented
 * by the existing `CallStringCTX` helper which truncates on overflow.
 *
 * The API mirrors the intraprocedural mono solver callbacks but extends the transfer
 * functions to receive the predecessor context when computing IN. GEN/KILL are
 * still computed per-instruction (context-insensitive) which matches the
 * standard call-string formulation for monotone frameworks.
 *
 * Currently only forward analyses are provided; backward support can be plugged
 * in using the same building blocks if needed.
 */

#ifndef ANALYSIS_MONO_SOLVER_CALLSTRING_INTERPROCEDURAL_DATAFLOW_H_
#define ANALYSIS_MONO_SOLVER_CALLSTRING_INTERPROCEDURAL_DATAFLOW_H_

#include "Dataflow/Mono/MonoFramework.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <deque>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_set>
#include <vector>

namespace dataflow {

/**
 * Context sensitive data-flow result.
 *
 * GEN/KILL are keyed only by Instruction* (shared across contexts).
 * IN/OUT are keyed by the pair (Instruction*, CallString).
 */
template <unsigned K, typename ContainerT> class ContextSensitiveDataFlowResult {
public:
  using Context = CallStringCTX<llvm::Instruction *, K>;

  struct ContextKey {
    llvm::Instruction *Inst;
    Context Ctx;

    bool operator<(const ContextKey &Rhs) const {
      if (Inst != Rhs.Inst) {
        return Inst < Rhs.Inst;
      }
      return Ctx < Rhs.Ctx;
    }
  };

  ContextSensitiveDataFlowResult() = default;

  ContainerT &GEN(llvm::Instruction *Inst) { return Gens[Inst]; }
  ContainerT &KILL(llvm::Instruction *Inst) { return Kills[Inst]; }

  ContainerT &IN(const ContextKey &Key) { return Ins[Key]; }
  ContainerT &OUT(const ContextKey &Key) { return Outs[Key]; }
  ContainerT &IN(llvm::Instruction *Inst, const Context &Ctx) {
    return IN(ContextKey{Inst, Ctx});
  }
  ContainerT &OUT(llvm::Instruction *Inst, const Context &Ctx) {
    return OUT(ContextKey{Inst, Ctx});
  }

  const ContainerT &IN(const ContextKey &Key) const {
    auto It = Ins.find(Key);
    if (It == Ins.end()) {
      static ContainerT EmptySet;
      return EmptySet;
    }
    return It->second;
  }

  const ContainerT &OUT(const ContextKey &Key) const {
    auto It = Outs.find(Key);
    if (It == Outs.end()) {
      static ContainerT EmptySet;
      return EmptySet;
    }
    return It->second;
  }
  const ContainerT &IN(llvm::Instruction *Inst, const Context &Ctx) const {
    return IN(ContextKey{Inst, Ctx});
  }
  const ContainerT &OUT(llvm::Instruction *Inst, const Context &Ctx) const {
    return OUT(ContextKey{Inst, Ctx});
  }

  bool hasContext(const ContextKey &Key) const {
    return Ins.find(Key) != Ins.end() || Outs.find(Key) != Outs.end();
  }

private:
  std::map<llvm::Instruction *, ContainerT> Gens;
  std::map<llvm::Instruction *, ContainerT> Kills;
  std::map<ContextKey, ContainerT> Ins;
  std::map<ContextKey, ContainerT> Outs;
};

/**
 * Call-string inter-procedural forward engine.
 *
 * K bounds the call-string length.
 */
template <unsigned K, typename ContainerT>
class CallStringInterProceduralDataFlowEngine {
public:
  using ResultTy = ContextSensitiveDataFlowResult<K, ContainerT>;
  using Context = typename ResultTy::Context;
  using ContextKey = typename ResultTy::ContextKey;

  CallStringInterProceduralDataFlowEngine() = default;

  /**
   * Forward call-string analysis rooted at `Entry`.
   *
   * - computeGEN/KILL: per-instruction (context-insensitive) transformers.
   * - initializeIN/initializeOUT: called when a (Inst, Ctx) pair is first seen.
   * - computeIN: merges predecessor OUT into IN. Receives predecessor context.
   * - computeOUT: updates OUT for the current node using its IN/GEN/KILL/etc.
   */
  ResultTy *applyForward(
      llvm::Function *Entry,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)>
          initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, ContainerT &IN,
                         ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx, ContainerT &OUT,
                         ResultTy *DF)>
          computeOUT);

  /**
   * Convenience overload with empty KILL sets.
   */
  ResultTy *applyForward(
      llvm::Function *Entry,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)>
          initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, ContainerT &IN,
                         ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx, ContainerT &OUT,
                         ResultTy *DF)>
          computeOUT) {
    auto EmptyKill = [](llvm::Instruction *, ResultTy *) {};
    return applyForward(Entry, computeGEN, EmptyKill, initializeIN,
                        initializeOUT, computeIN, computeOUT);
  }

private:
  using WorkQueue = std::deque<ContextKey>;

  static bool isCallToDefinedFunction(llvm::Instruction *Inst);
  static llvm::Function *getDirectCallee(llvm::Instruction *Inst);

  static std::vector<llvm::Instruction *>
  getReturnInstructions(llvm::Function *F);

  static llvm::Instruction *
  getFirstInstruction(llvm::BasicBlock *BB) {
    return &*BB->begin();
  }

  static std::vector<llvm::Instruction *>
  normalSuccessors(llvm::Instruction *Inst);

  static std::vector<llvm::Instruction *>
  normalPredecessors(llvm::Instruction *Inst);

  static std::vector<llvm::Instruction *>
  continuationInstructions(llvm::Instruction *CallInst);

  static bool isFunctionEntry(llvm::Instruction *Inst) {
    auto *BB = Inst->getParent();
    return &BB->getParent()->getEntryBlock() == BB &&
           Inst == &*BB->begin();
  }

  void computeGenKill(llvm::Module *M,
                      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
                      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
                      ResultTy *DF);

  void ensureInitialized(const ContextKey &Key,
                         std::function<void(llvm::Instruction *, ContainerT &)>
                             initializeIN,
                         std::function<void(llvm::Instruction *, ContainerT &)>
                             initializeOUT,
                         ResultTy *DF);

  std::vector<ContextKey>
  predecessors(
      const ContextKey &Key,
      const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> &CallToReturns,
      const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> &ContinuationToCalls);

  std::vector<ContextKey>
  successors(const ContextKey &Key);
};

} // namespace dataflow

#endif // ANALYSIS_MONO_SOLVER_CALLSTRING_INTERPROCEDURAL_DATAFLOW_H_
