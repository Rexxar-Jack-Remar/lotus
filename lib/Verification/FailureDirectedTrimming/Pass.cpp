// FailureDirectedTrimmingPass: Implements failure-directed program trimming
// (Ferles et al., ESEC/FSE'17) as an LLVM IR instrumentation pass.

#include "Verification/FailureDirectedTrimming/FailureDirectedTrimming.h"
#include "FailureDirectedTrimmingImpl.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>

#include <iterator>

using namespace llvm;

bool runFailureDirectedTrimming(Module &M) {
  bool Changed = false;

  FunctionCallee AssumeFn = getVerifierAssume(M);
  NondetFactory Nondet(M);
  DerefUFFactory DerefUF(M);

  DenseMap<Function *, Function *> SafeOf = cloneSafeFunctions(M, AssumeFn);
  if (!SafeOf.empty())
    Changed = true;

  Changed |= wrapCallsInOriginalFunctions(M, AssumeFn, SafeOf, Nondet);

  auto isFailContextCall = [&](const CallBase &CB) -> bool {
    const BasicBlock *BB = CB.getParent();
    if (!BB)
      return false;
    if (!isa<UnreachableInst>(BB->getTerminator()))
      return false;

    for (auto It = std::next(CB.getIterator()); It != BB->end(); ++It) {
      const Instruction *I = &*It;
      if (isa<DbgInfoIntrinsic>(I))
        continue;
      const auto *NextCB = dyn_cast<CallBase>(I);
      if (!NextCB)
        return false;
      Function *CF = getDirectCalledFunction(*NextCB);
      if (!CF || !isAssumeFunctionName(CF->getName()))
        return false;
      if (NextCB->arg_size() < 1)
        return false;
      if (auto *C = dyn_cast<ConstantInt>(NextCB->getArgOperand(0))) {
        return C->isZero();
      }
      return false;
    }
    return false;
  };

  DenseSet<const Function *> DoNotInstrument;
  bool HasIndirectCalls = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;

    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Function *CF = getDirectCalledFunction(*CB);
      if (!CF) {
        if (!CB->isInlineAsm())
          HasIndirectCalls = true;
        continue;
      }
      if (!SafeOf.count(CF))
        continue;
      if (!isFailContextCall(*CB))
        DoNotInstrument.insert(CF);
    }
  }

  if (HasIndirectCalls) {
    for (auto &KV : SafeOf) {
      Function *F = KV.first;
      if (F && F->hasAddressTaken())
        DoNotInstrument.insert(F);
    }
  }

  lotus::AAConfig AAConfig =
      lotus::parseAAConfigFromString(FDTrimAA, lotus::AAConfig::SeaDsa());
  lotus::AliasAnalysisWrapper AA(M, AAConfig);

  HasAsrtsEnv Has = computeHasAsrts(M);

  ExprFactory EF(M.getContext());
  BoundVarManager BVM;
  SummaryEnv Env;

  if (FDTrimSummaryIterations > 0) {
    for (unsigned It = 0; It < FDTrimSummaryIterations; ++It) {
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        if (F.getName().endswith(".fdtrim.safe"))
          continue;

        FunctionSCResult R =
            computeSafetyConditions(F, EF, BVM, AA, Env, Has);
        Env.Summaries[&F] = R.Summary;
      }
    }
  }

  std::set<const Instruction *> Instrumented;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;
    if (DoNotInstrument.count(&F))
      continue;

    FunctionSCResult R = computeSafetyConditions(F, EF, BVM, AA, Env, Has);

    std::vector<Instruction *> Points;
    Points.reserve(64);

    if (FDTrimInstrumentCalls) {
      for (Instruction &I : instructions(F)) {
        if (isa<DbgInfoIntrinsic>(&I))
          continue;
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        if (Function *CF = getDirectCalledFunction(*CB)) {
          if (isAssumeFunctionName(CF->getName()) ||
              isAssumeNotFunctionName(CF->getName()) ||
              isAssertFunctionName(CF->getName()) ||
              isErrorFunctionName(CF->getName()) ||
              isNondetFunctionName(CF->getName()))
            continue;
        }
        Points.push_back(&I);
      }
    }

    if (FDTrimInstrumentConditionals) {
      for (BasicBlock &BB : F) {
        Instruction *T = BB.getTerminator();
        if (isa<BranchInst>(T) || isa<SwitchInst>(T))
          Points.push_back(T);
      }
    }

    if (FDTrimInstrumentLoops) {
      DominatorTree DT;
      DT.recalculate(F);
      LoopInfo LI;
      LI.analyze(DT);
      std::function<void(Loop *)> Visit = [&](Loop *L) {
        if (!L)
          return;
        if (BasicBlock *Header = L->getHeader()) {
          Instruction *InsPt = firstNonPhiNonDbg(*Header);
          Points.push_back(InsPt);
        }
        for (Loop *Sub : L->getSubLoops())
          Visit(Sub);
      };
      for (Loop *L : LI)
        Visit(L);
    }

    std::sort(Points.begin(), Points.end());
    Points.erase(std::unique(Points.begin(), Points.end()), Points.end());

    for (Instruction *P : Points) {
      if (!P || !P->getParent())
        continue;
      if (Instrumented.count(P))
        continue;
      auto It = R.BeforeInst.find(P);
      if (It == R.BeforeInst.end())
        continue;

      ExprRef Safety = It->second;
      ExprRef TrimCond = negateForTrimming(EF, Safety);
      TrimCond = boundConjuncts(EF, TrimCond, FDTrimMaxConjuncts);

      if (FDTrimQuantElim == "z3") {
        if (ExprRef QE = tryEliminateExistsByZ3QE(EF, M, TrimCond))
          TrimCond = QE;
      }

      if (TrimCond && TrimCond->Kind == ExprKind::BoolConst &&
          TrimCond->BoolVal) {
        Instrumented.insert(P);
        continue;
      }

      IRBuilder<> B(P);
      DenseMap<uint32_t, Value *> BoundVals;
      ExprRef NoExists =
          eliminateExistsByNondet(EF, TrimCond, B, M, Nondet, BoundVals);
      Value *CondV =
          codegenValue(EF, NoExists, B, BoundVals, M, Nondet, DerefUF);
      if (!CondV)
        continue;
      if (auto *C = dyn_cast<ConstantInt>(CondV)) {
        if (C->isOne()) {
          Instrumented.insert(P);
          continue;
        }
      }

      B.CreateCall(AssumeFn, CondV);
      Instrumented.insert(P);
      Changed = true;
    }
  }

  return Changed;
}

PreservedAnalyses FailureDirectedTrimmingPass::run(Module &M,
                                                   ModuleAnalysisManager &) {
  bool Changed = runFailureDirectedTrimming(M);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {

class FailureDirectedTrimmingLegacyPass : public ModulePass {
public:
  static char ID;
  FailureDirectedTrimmingLegacyPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    return runFailureDirectedTrimming(M);
  }
};

char FailureDirectedTrimmingLegacyPass::ID = 0;
static RegisterPass<FailureDirectedTrimmingLegacyPass> X(
    "fdtrim", "Failure-directed program trimming", false, false);

} // namespace
