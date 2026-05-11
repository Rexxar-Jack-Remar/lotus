//===----------------------------------------------------------------------===//
//
// AnalysisState taint analysis implementation.
// This file is the taint-specific half of AnalysisState. It injects source
// facts, threads them through symbolic values and modeled calls, then exports
// summary information so callers can continue the same taint story.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"
#include "Analysis/SymbolicExecution/AnalysisState.h"
#include "Analysis/SymbolicExecution/DomTreePass.h"
#include "Analysis/SymbolicExecution/InstResolver.h"
#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/PropertyAllocator.h"
#include "Analysis/SymbolicExecution/PropertyInteger.h"
#include "Analysis/SymbolicExecution/PropertySym.h"
#include "Analysis/SymbolicExecution/TaintModel.h"

#include <functional>
#include <numeric>

#define DEBUG_TYPE "Symex"

#ifndef MTDEBUG
#define MTDEBUG(X) LLVM_DEBUG(X)
#endif

using namespace SymbolicExecution;

void AnalysisState::taintInit(Function *Func) {
  // Seed taint only at well-known entry boundaries. The rest of the engine
  // treats these marks like any other guarded symbolic fact, so the special
  // handling stays local to setup.
  if (Func->getName() == "main") {
    auto &EntryBB = Func->getEntryBlock();

    if (EntryBB.empty()) {
      return;
    }

    auto ArgList = Func->args();
    // define i64 @main(i64 %argc, i8** %argv)
    if (Func->arg_size() >= 2) {
      auto *Iter = ArgList.begin();
      ++Iter;

      Argument *Arg = &*Iter;
      if (Arg->getType()->isPointerTy() &&                          // i8 **
          Arg->getType()->getPointerElementType()->isPointerTy() && // i8 *
          Arg->getType()
              ->getPointerElementType()
              ->getPointerElementType()
              ->isIntegerTy(8) // i8
      ) {
        // taint argv
        taintVal(getNode(Arg), {}, Condition());
      }
    }
  }
}

void AnalysisState::taintTransfer(Instruction *Inst) {
  auto OpC = Inst->getOpcode();
  if (OpC == Instruction::Call) {
    if (!gvfg_utility::isDefiniteCall(Inst)) {
      processCallTaintSources(Inst);
      processTaintPropagation(Inst);
    }
  } else {
    processTaintPropagation(Inst);
  }
}

void AnalysisState::processCallTaintSources(Instruction *Inst) {
  // Declarations are where the taint model injects external source facts.
  // Defined callees are handled by normal summary import instead, which keeps
  // the source specification focused on library and environment boundaries.
  Function *CalleeFunc = gvfg_utility::getCallee(Inst);
  if (!CalleeFunc || !CalleeFunc->isDeclaration()) {
    return;
  }

  std::set<Var> TaintedDsts = gvfg_utility::getTaintedVars(Inst, TaintSpec);
  if (TaintedDsts.empty()) {
    return;
  }

  MTDEBUG(errs() << "[DEBUG-Numerical] Found Taint source in func "
                 << F->getName() << ":\n");
  MTDEBUG(Inst->print(llvm::dbgs(), true); llvm::dbgs() << "\n";);

  Condition BBCond = getLocalCond(Inst->getParent());
  for (const auto &V : TaintedDsts) {

    auto *SrcVal = V.getValue().getLLVMVal();
    assert(SrcVal);
    TaintStep SrcStep(TaintStep::TAINT_STEP_SOURCE, Inst, SrcVal);
    taintVal(V.getValue(), {SrcStep}, BBCond);
  }
}

void AnalysisState::taintVal(const ProgramValuePtr &V,
                             const std::vector<TaintStep> &Steps,
                             const Condition &PreCond, bool Peel) {
  // markTaint records the explicit destination first. The optional peeling step
  // then walks symbolic definitions so taint follows the variables that explain
  // the current value, not just the surface GVFG node that received it.
  markTaint(V, Steps, PreCond);

  if (!Peel) {
    return;
  }

  if (!hasSymbolicVals(V)) {
    return;
  }

  const auto &Vals = getSymbolicVals(V);
  for (const auto &P : Vals) {
    const auto &Val = P.first;
    const auto &Cond = P.second;
    if (IsaProperty<PropertySymExpr>(Val)) {
      const auto *SymVal = CastProperty<PropertySymExpr>(Val);

      bool ShouldPropagate = false;
      if (V.getType()->isIntegerTy()) {
        ShouldPropagate = (SymVal->getUsedVars().size() == 1);
      } else if (V.getType()->isPointerTy()) {
        // This means the pointer = base_pointer + offset
        //
        // In this case, the taint should propagate from "pointer" to
        // "base_pointer" if the offset is zero (e.g., gep 0 0) or
        // symbolic (e.g., symbolic offsets into an array) On the other
        // hand, if the offset is a constant non-zero value, we do not
        // propagate to the base pointer. Example: struct st {..., int
        // ar[100]; }, taint ar should not taint the entire struct.
        ShouldPropagate = (SymVal->getUsedVars().size() > 1) || SymVal->isVar();
      }

      for (const auto &DstV : SymVal->getUsedVars()) {
        if (!ShouldPropagate) {
          continue;
        }

        if (V.getType()->isIntegerTy()) {
          markTaint(DstV.getValue(), Steps, Cond && PreCond);
        } else if (DstV.getValue().getType()->isPointerTy()) { // V is a pointer
          // For cases like gep: p = q + off,
          // taint(p) shall taint(q), but should not mark off as being
          // tainted.
          markTaint(DstV.getValue(), Steps, Cond && PreCond);
        }
      }
    }
  }
}

void AnalysisState::markTaint(const ProgramValuePtr &V,
                              const std::vector<TaintStep> &Steps,
                              const Condition &Cond) {
  if (V.isConstant()) {
    return;
  }

  if (!TaintedSteps.count(V)) {
    TaintedSteps[V] = Steps;
  }

  TaintedVals.addValue(V, Cond);

  MTDEBUG(errs() << "[DEBUG-Numerical] propagate taint in func " << F->getName()
                 << ":\n");
  MTDEBUG(V.dump());
}

void AnalysisState::processTaintPropagation(Instruction *Inst) {
  if (isa<CmpInst>(Inst)) {
    return;
  }

  Condition BBCond;
  if (&*Inst->getParent()->begin() == Inst) {
    BBCond = getLocalCond(Inst->getParent());
  }

  auto OpC = Inst->getOpcode();
  if (OpC == Instruction::Call) {
    auto TaintedTransfers = getTaintTransferTargets(Inst);

    for (const auto &P : TaintedTransfers) {
      const auto &Src = P.first;
      for (const auto &Dst : P.second) {
        propagateTaint(Src, Dst, Inst, BBCond);
      }
    }
    return;
  }

  if (Inst->getType()->isVoidTy()) {
    return;
  }

  // For non-call instructions, taint follows the same symbolic dataflow that
  // builds scalar expressions. That keeps taint aligned with the guarded value
  // set instead of inventing a separate transfer relation.
  ProgramValuePtr DstV(getNode(Inst));
  bool IsDstPointer = DstV.getType()->isPointerTy();

  if (isInstUnmodelled(Inst)) {
    for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
      auto OpndVals = getSymbolicVals(getNode(Inst->getOperand(Idx)));
      for (const auto &P : OpndVals) {
        const auto &OpndVal = P.first;
        if (IsaProperty<PropertySymExpr>(OpndVal)) {
          for (const auto &DepV :
               CastProperty<PropertySymExpr>(OpndVal)->getUsedVars()) {
            propagateTaint(DepV.getValue(), DstV, Inst, P.second && BBCond);
          }
        }
      }
    }
    return;
  }

  for (const auto &P : getSymbolicVals(DstV)) {
    const auto &SymVal = P.first;
    if (IsaProperty<PropertyInteger>(SymVal))
      continue;
    Condition PreCond = BBCond && P.second;
    for (const auto &Sym :
         CastProperty<PropertySymExpr>(SymVal)->getUsedVars()) {
      auto SrcV = Sym.getValue();
      bool ValuePassing = !IsDstPointer || SrcV.getType()->isPointerTy();
      if (ValuePassing) {
        propagateTaint(SrcV, DstV, Inst, PreCond);
      }
    }
  }
}

void AnalysisState::propagateTaint(const ProgramValuePtr &Src,
                                   const ProgramValuePtr &Dst,
                                   Instruction *Inst, const Condition &Cond,
                                   bool Peel) {
  // Taint only moves along paths where the source is already reachable. The
  // propagated condition therefore intersects the caller-supplied edge guard
  // with the source's accumulated taint guard.
  Condition PreCond = getTaintedCond(Src) && Cond;
  std::vector<TaintStep> Steps;
  if (!PreCond.isFalse()) {
    if (TaintedSteps.count(Src)) {
      Steps = TaintedSteps.at(Src);
    }

    if (Dst.isa<GuardedValueFlowNodeValue>() &&
        Src.isa<GuardedValueFlowNodeValue>() && Inst) {
      Steps.emplace_back(TaintStep(TaintStep::TAINT_STEP_PROP, Inst,
                                   Src.getLLVMVal(), Dst.getLLVMVal()));
    }

    taintVal(Dst, Steps, PreCond, Peel);
  }
}

std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>>
AnalysisState::getTaintTransferTargets(Instruction *Inst) const {
  std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>> Res;
  auto *CS = dyn_cast<CallBase>(Inst);
  if (!CS) {
    return Res;
  }

  // At call sites we first ask the taint specification for explicit source to
  // destination transfers. If no model applies, fall back to a narrow set of
  // memory intrinsics whose dataflow is simple enough to encode locally.
  assert(!gvfg_utility::isDefiniteCall(Inst));
  for (size_t Idx = 0; Idx < CS->arg_size(); ++Idx) {
    Value *ArgVal = CS->getArgOperand(Idx);
    if (!ArgVal) {
      continue;
    }

    ArgVal = ArgVal->stripPointerCasts();
    auto *ArgNode = Graph->findNode(ArgVal);
    if (!ArgNode) {
      // Unknown to the GVFG, skip to avoid crashing.
      continue;
    }

    ProgramValuePtr Arg(ArgNode);
    std::vector<Value *> DstVect;
    if (TaintSpec) {
      TaintSpec->getTransferDstVect(CS, ArgNode->getLLVMValue(), DstVect);
    }

    for (auto *V : DstVect) {
      if (!V) {
        continue;
      }

      V = V->stripPointerCasts();
      if (auto *DstNode = Graph->findNode(V)) {
        Res[Arg].insert(ProgramValuePtr(DstNode));
      }
    }
  }

  if (!Res.empty()) {
    return Res;
  }

  Function *Callee = gvfg_utility::getCallee(Inst);
  if (Callee && Callee->isIntrinsic()) {
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::memset:
    case Intrinsic::memmove:
    case Intrinsic::memcpy:
      Res[getNode(CS->getArgOperand(1))].insert(getNode(CS->getArgOperand(0)));
    }
  }

  return Res;
}

void AnalysisState::propagateTaintPointer(const ProgramValuePtr &LdPtr,
                                          const ProgramValuePtr &LdVal,
                                          Instruction *Inst,
                                          const Condition &Cond) {
  // Loads often read through derived pointers. We taint the loaded value from
  // the pointer itself, then also from pointer-producing symbolic operands so a
  // zero-offset GEP style derivation still carries the source object taint.
  propagateTaint(LdPtr, LdVal, Inst, Cond);

  const auto &Vals = getSymbolicVals(LdPtr);
  for (const auto &P : Vals) {
    const auto &Val = P.first;
    const auto &ValCond = P.second;
    if (IsaProperty<PropertySymExpr>(Val)) {
      const auto *SymVal = CastProperty<PropertySymExpr>(Val);

      bool ShouldPropagate =
          (SymVal->getUsedVars().size() > 1) || SymVal->isVar();
      if (ShouldPropagate) {
        for (const auto &SrcV : SymVal->getUsedVars()) {
          if (SrcV.getValue().getType()->isPointerTy()) {
            propagateTaint(SrcV.getValue(), LdVal, Inst, Cond && ValCond);
          }
        }
      }
    }
  }
}

Condition AnalysisState::getTaintedCond(const ProgramValuePtr &V) const {
  if (TaintedVals.hasValue(V)) {
    return TaintedVals.getGuardForValue(V);
  } else {
    return Condition::getFalseCond();
  }
}

void AnalysisState::buildTaintSummary() {
  // Summaries only expose the taint that crosses function boundaries. Internal
  // temporaries stay inside AnalysisState, while guarded formals, returns, and
  // their step histories are preserved for caller-side replay.
  TaintValSet TaintedFormals;
  TaintValSet TaintedRets;

  for (auto Iter = Graph->arg_begin(), EIter = Graph->arg_end(); Iter != EIter;
       ++Iter) {
    ProgramValuePtr Arg(*Iter);
    TaintedFormals.addValue(Arg, getTaintedCond(Arg));
  }

  for (auto Iter = Graph->return_begin(), EIter = Graph->return_end();
       Iter != EIter; ++Iter) {
    ProgramValuePtr Ret(*Iter);
    TaintedRets.addValue(Ret, getTaintedCond(Ret));
  }

  TaintSmry = {TaintedFormals, TaintedRets, TaintedSteps};
}

void AnalysisState::taintProcessCall(CallInst *Inst, Function *Callee,
                                     const TaintSummary &Smry) {
  // Importing a callee summary is the dual of buildTaintSummary. Conditions are
  // translated into the caller context, then the callee's step history gains a
  // call-boundary marker before it is attached to actual arguments or returns.
  const auto &FormalToReal = FormalToRealMap.at(Inst);
  const auto &CalleeTaintedSteps = Smry.TaintedSteps;
  auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(Inst);

  Condition BBCond = getLocalCond(Inst->getParent());
  for (const auto &P : Smry.TaintedFormals) {
    if (!FormalToReal.count(P.first)) {
      continue;
    }

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    auto Steps = CalleeTaintedSteps.at(P.first);
    Steps.emplace_back(TaintStep(TaintStep::TAINT_STEP_CALL, Inst));
    taintVal(FormalToReal.at(P.first), Steps, Cond);
  }

  for (const auto &P : Smry.TaintedRets) {
    auto *RetNode = P.first.getAs<GuardedValueFlowNodeValue>()->getNode();
    const GuardedValueFlowNode *OutNode = nullptr;

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    auto Steps = CalleeTaintedSteps.at(P.first);
    Steps.emplace_back(TaintStep(TaintStep::TAINT_STEP_CALL, Inst));

    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    if (OutNode) {
      taintVal(OutNode, Steps, Cond);
    }
  }
}
