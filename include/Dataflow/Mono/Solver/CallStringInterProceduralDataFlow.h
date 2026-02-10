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

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/Mono/Contexts/CallStringCTX.h"
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
  using Context = mono::CallStringCTX<llvm::Instruction *, K>;

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

  const std::map<ContextKey, ContainerT> &getINMap() const {
    return Ins;
  }

  const std::map<ContextKey, ContainerT> &getOUTMap() const {
    return Outs;
  }

  const std::map<llvm::Instruction *, ContainerT> &getGENMap() const {
    return Gens;
  }

  const std::map<llvm::Instruction *, ContainerT> &getKILLMap() const {
    return Kills;
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
  using ICFG = dataflow::controlflow::InterCFG;

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
      const ICFG *ICF,
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
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal,
      std::function<std::vector<llvm::Function *>(llvm::Instruction *)>
          getCalleesOfCallAt);

  /// Seed-based variant (Phasar-like): starts the fixpoint from an explicit set
  /// of (Inst, Ctx) keys and can inject initial facts for those keys.
  ResultTy *applyForwardFromSeeds(
      llvm::Module *M, const std::vector<ContextKey> &Seeds,
      const ICFG *ICF,
      const std::map<ContextKey, ContainerT> &SeedIns,
      std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
      std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
      std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
      std::function<void(llvm::Instruction *Inst, ContainerT &OUT)>
          initializeOUT,
      std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                         const Context &PredCtx, ContainerT &IN, ResultTy *DF)>
          computeIN,
      std::function<void(llvm::Instruction *Inst, const Context &Ctx, ContainerT &OUT,
                         ResultTy *DF)>
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal,
      std::function<std::vector<llvm::Function *>(llvm::Instruction *)>
          getCalleesOfCallAt);

  /**
   * Convenience overload with empty KILL sets.
   */
  ResultTy *applyForward(
      llvm::Function *Entry,
      const ICFG *ICF,
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
          computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal,
      std::function<std::vector<llvm::Function *>(llvm::Instruction *)>
          getCalleesOfCallAt) {
    auto EmptyKill = [](llvm::Instruction *, ResultTy *) {};
    return applyForward(Entry, ICF, computeGEN, EmptyKill, initializeIN, initializeOUT,
                        computeIN, computeOUT, std::move(equal),
                        std::move(getCalleesOfCallAt));
  }

private:
  using WorkQueue = std::deque<ContextKey>;

  static bool isCallToDefinedFunction(
      llvm::Instruction *Inst, const ICFG *ICF);

  static llvm::Instruction *
  getFirstInstruction(llvm::BasicBlock *BB) {
    return &*BB->begin();
  }

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
      const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> &ContinuationToCalls,
      const ICFG *ICF);

  std::vector<ContextKey>
  successors(const ContextKey &Key,
             const ICFG *ICF);
};

// ---- Header-only template implementation ----

template <unsigned K, typename ContainerT>
bool CallStringInterProceduralDataFlowEngine<K, ContainerT>::isCallToDefinedFunction(
    llvm::Instruction *Inst, const ICFG *ICF) {
  if (ICF == nullptr || Inst == nullptr || !ICF->isCallSite(Inst)) {
    return false;
  }
  for (auto *Callee : ICF->getCalleesOfCallAt(Inst)) {
    if (Callee != nullptr && !Callee->isDeclaration() && !Callee->empty()) {
      return true;
    }
  }
  return false;
}

template <unsigned K, typename ContainerT>
void CallStringInterProceduralDataFlowEngine<K, ContainerT>::computeGenKill(
    llvm::Module *M,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    ResultTy *DF) {
  for (auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        computeGEN(&I, DF);
        computeKILL(&I, DF);
      }
    }
  }
}

template <unsigned K, typename ContainerT>
void CallStringInterProceduralDataFlowEngine<K, ContainerT>::ensureInitialized(
    const ContextKey &Key,
    std::function<void(llvm::Instruction *, ContainerT &)> initializeIN,
    std::function<void(llvm::Instruction *, ContainerT &)> initializeOUT,
    ResultTy *DF) {
  if (DF->hasContext(Key)) {
    return;
  }
  auto &INSet = DF->IN(Key);
  auto &OUTSet = DF->OUT(Key);
  initializeIN(Key.Inst, INSet);
  initializeOUT(Key.Inst, OUTSet);
}

template <unsigned K, typename ContainerT>
std::vector<typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ContextKey>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::successors(
    const ContextKey &Key,
    const ICFG *ICF) {
  std::vector<ContextKey> Result;
  auto *Inst = Key.Inst;
  auto Ctx = Key.Ctx;

  if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Inst)) {
    if (!Ctx.empty()) {
      auto CallerCtx = Ctx;
      auto *CallInst = CallerCtx.pop_back();
      for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
        Result.push_back({Cont, CallerCtx});
      }
    } else if (ICF != nullptr) {
      // Phasar-like: empty context at an exit propagates to all callers.
      for (auto *CallInst : ICF->getCallersOf(Ret->getFunction())) {
        for (auto *Cont : ICF->getReturnSitesOfCallAt(CallInst)) {
          Result.push_back({Cont, Ctx});
        }
      }
    }
    return Result;
  }

  if (isCallToDefinedFunction(Inst, ICF)) {
    // Call-to-return successors always exist (even for defined callees).
    for (auto *SuccInst : ICF->getReturnSitesOfCallAt(Inst)) {
      Result.push_back({SuccInst, Ctx});
    }

    // Call edges to all defined callees.
    for (auto *Callee : ICF->getCalleesOfCallAt(Inst)) {
      if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
        continue;
      }
      Context CalleeCtx = Ctx;
      CalleeCtx.push_back(Inst);
      for (auto *Start : ICF->getStartPointsOf(Callee)) {
        Result.push_back({Start, CalleeCtx});
      }
    }
    return Result;
  }

  for (auto *SuccInst : ICF->getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
    Result.push_back({SuccInst, Ctx});
  }
  return Result;
}

template <unsigned K, typename ContainerT>
std::vector<typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ContextKey>
CallStringInterProceduralDataFlowEngine<K, ContainerT>::predecessors(
    const ContextKey &Key,
    const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> &CallToReturns,
    const std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> &ContinuationToCalls,
    const ICFG *ICF) {
  std::vector<ContextKey> Result;
  auto *Inst = Key.Inst;
  auto Ctx = Key.Ctx;

  if (isFunctionEntry(Inst)) {
    if (!Ctx.empty()) {
      auto CallerCtx = Ctx;
      auto *CallInst = CallerCtx.pop_back();
      Result.push_back({CallInst, CallerCtx});
    }
    return Result;
  }

  auto ContIt = ContinuationToCalls.find(Inst);
  if (ContIt != ContinuationToCalls.end()) {
    for (auto *CallInst : ContIt->second) {
      auto CallToRetIt = CallToReturns.find(CallInst);
      if (CallToRetIt == CallToReturns.end()) {
        continue;
      }
      Context RetCtx = Ctx;
      RetCtx.push_back(CallInst);
      for (auto *RetInst : CallToRetIt->second) {
        Result.push_back({RetInst, RetCtx});
        if (Ctx.empty()) {
          // Phasar-like: allow returns to flow back even for empty contexts.
          Result.push_back({RetInst, Ctx});
        }
      }
    }
  }

  for (auto *PredInst : ICF->getPredsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
    Result.push_back({PredInst, Ctx});
  }
  return Result;
}

template <unsigned K, typename ContainerT>
typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy *
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyForward(
    llvm::Function *Entry,
    const ICFG *ICF,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx, ContainerT &OUT,
                       ResultTy *DF)>
        computeOUT,
      std::function<bool(const ContainerT &, const ContainerT &)> equal,
      std::function<std::vector<llvm::Function *>(llvm::Instruction *)>
          getCalleesOfCallAt) {
  if (Entry == nullptr || Entry->isDeclaration()) {
    return nullptr;
  }

  auto *Module = Entry->getParent();
  Context EmptyCtx;
  ContextKey EntryKey{&*Entry->getEntryBlock().begin(), EmptyCtx};
  std::vector<ContextKey> Seeds{EntryKey};
  std::map<ContextKey, ContainerT> SeedIns;
  return applyForwardFromSeeds(Module, Seeds, ICF, SeedIns, std::move(computeGEN),
                               std::move(computeKILL), std::move(initializeIN),
                               std::move(initializeOUT), std::move(computeIN),
                               std::move(computeOUT), std::move(equal),
                               std::move(getCalleesOfCallAt));
}

template <unsigned K, typename ContainerT>
typename CallStringInterProceduralDataFlowEngine<K, ContainerT>::ResultTy *
CallStringInterProceduralDataFlowEngine<K, ContainerT>::applyForwardFromSeeds(
    llvm::Module *M, const std::vector<ContextKey> &Seeds, const ICFG *ICF,
    const std::map<ContextKey, ContainerT> &SeedIns,
    std::function<void(llvm::Instruction *, ResultTy *)> computeGEN,
    std::function<void(llvm::Instruction *, ResultTy *)> computeKILL,
    std::function<void(llvm::Instruction *Inst, ContainerT &IN)> initializeIN,
    std::function<void(llvm::Instruction *Inst, ContainerT &OUT)> initializeOUT,
    std::function<void(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                       const Context &PredCtx, ContainerT &IN, ResultTy *DF)>
        computeIN,
    std::function<void(llvm::Instruction *Inst, const Context &Ctx, ContainerT &OUT,
                       ResultTy *DF)>
        computeOUT,
    std::function<bool(const ContainerT &, const ContainerT &)> equal,
    std::function<std::vector<llvm::Function *>(llvm::Instruction *)>
        getCalleesOfCallAt) {
  if (M == nullptr || Seeds.empty() || ICF == nullptr) {
    return nullptr;
  }

  auto *DF = new ResultTy();
  computeGenKill(M, computeGEN, computeKILL, DF);

  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> CallToReturns;
  std::map<llvm::Instruction *, std::vector<llvm::Instruction *>> ContinuationToCalls;
  for (auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (ICF->isCallSite(&I)) {
          for (auto *Cont : ICF->getReturnSitesOfCallAt(&I)) {
            ContinuationToCalls[Cont].push_back(&I);
          }
        }

        if (isCallToDefinedFunction(&I, ICF)) {
          std::vector<llvm::Instruction *> Returns;
          for (auto *Callee : ICF->getCalleesOfCallAt(&I)) {
            auto CalleeReturns = ICF->getExitPointsOf(Callee);
            Returns.insert(Returns.end(), CalleeReturns.begin(),
                           CalleeReturns.end());
          }
          CallToReturns[&I] = std::move(Returns);
        }
      }
    }
  }

  WorkQueue Queue;
  std::set<ContextKey> InQueue;

  auto Enqueue = [&](const ContextKey &Key) {
    if (InQueue.insert(Key).second) {
      Queue.push_back(Key);
    }
  };

  for (const auto &Seed : Seeds) {
    Enqueue(Seed);
  }

  // Inject explicit IN seeds at empty context (Phasar-like).
  for (const auto &Seed : SeedIns) {
    ensureInitialized(Seed.first, initializeIN, initializeOUT, DF);
    DF->IN(Seed.first) = Seed.second;
  }

  while (!Queue.empty()) {
    ContextKey Current = Queue.front();
    Queue.pop_front();
    InQueue.erase(Current);

    ensureInitialized(Current, initializeIN, initializeOUT, DF);

    auto &InSet = DF->IN(Current);
    ContainerT OldIn = InSet;
    ContainerT NewIn;
    initializeIN(Current.Inst, NewIn);
    {
      auto SeedIt = SeedIns.find(Current);
      if (SeedIt != SeedIns.end()) {
        // Preserve explicit boundary facts while still recomputing predecessor
        // contributions from scratch.
        NewIn = SeedIt->second;
      }
    }

    for (const auto &PredKey :
         predecessors(Current, CallToReturns, ContinuationToCalls, ICF)) {
      ensureInitialized(PredKey, initializeIN, initializeOUT, DF);
      computeIN(Current.Inst, PredKey.Inst, PredKey.Ctx, NewIn, DF);
    }
    InSet = std::move(NewIn);

    auto &OutSet = DF->OUT(Current);
    ContainerT OldOut = OutSet;
    computeOUT(Current.Inst, Current.Ctx, OutSet, DF);

    if (!equal(OutSet, OldOut) || !equal(InSet, OldIn)) {
      for (const auto &SuccKey : successors(Current, ICF)) {
        Enqueue(SuccKey);
      }
    }
  }

  return DF;
}

} // namespace dataflow

#endif // ANALYSIS_MONO_SOLVER_CALLSTRING_INTERPROCEDURAL_DATAFLOW_H_
