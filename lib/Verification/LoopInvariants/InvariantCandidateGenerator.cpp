#include "Verification/LoopInvariants/InvariantCandidateGenerator.h"
#include "Verification/LoopInvariants/Z3ValueNaming.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
//#include "llvm/Support/raw_ostream.h"

#include <cstdint>

using namespace llvm;
using namespace lotus;

InvariantCandidateGenerator::InvariantCandidateGenerator(const Loop &Loop,
                                                         ScalarEvolution &SE,
                                                         LoopInfo &LI,
                                                         DominatorTree &DT)
    : L(Loop), SE(SE), LI(LI), DT(DT) {}

void InvariantCandidateGenerator::generateCandidates(
    SmallVectorImpl<InvariantCandidate> &Candidates) {

  analyzeInductionVariables();
  analyzeGEPInstructions();
  extractLoopBounds();
  collectAssignments();
  analyzeValueDeltas();
  inferTerminators();
  collectLoopComparisons();

  generateMonotonicityInvariants(Candidates);
  generateBoundInvariants(Candidates);
  generateLinearRelationInvariants(Candidates);
  generateAssignmentBasedInvariants(Candidates);
  generateTerminatorInvariants(Candidates);
  generateFlagBasedInvariants(Candidates);
}

void InvariantCandidateGenerator::analyzeGEPInstructions() {
  Module *M = L.getHeader()->getParent()->getParent();
  const DataLayout &DL = M->getDataLayout();

  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I);
      if (!GEP)
        continue;

      Value *Ptr = GEP->getPointerOperand();
      PHINode *PtrPhi = dyn_cast<PHINode>(Ptr);
      if (!PtrPhi || PtrPhi->getParent() != L.getHeader())
        continue;

      for (auto &IVInfo : InductionVars) {
        if (IVInfo.Phi != PtrPhi)
          continue;

        if (GEP->getNumIndices() == 1) {
          Value *Idx = GEP->getOperand(1);
          if (ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
            Type *ElementType = GEP->getResultElementType();
            uint64_t ElemSize = DL.getTypeAllocSize(ElementType);
            (void)CI;

            if (!IVInfo.IsPointerInduction) {
              const_cast<InductionVariableInfo &>(IVInfo).IsPointerInduction =
                  true;
              const_cast<InductionVariableInfo &>(IVInfo).PointerElementType =
                  ElementType;
              const_cast<InductionVariableInfo &>(IVInfo).ElementSize =
                  ElemSize;
            }
          }
        }
      }
    }
  }
}

void InvariantCandidateGenerator::analyzeInductionVariables() {
  BasicBlock *Header = L.getHeader();
  if (!Header)
    return;

  Module *M = Header->getParent()->getParent();
  const DataLayout &DL = M->getDataLayout();

  for (auto &Inst : *Header) {
    PHINode *Phi = dyn_cast<PHINode>(&Inst);
    if (!Phi)
      break;

    Type *PhiType = Phi->getType();
    if (!PhiType->isIntegerTy() && !PhiType->isPointerTy())
      continue;

    const SCEV *S = SE.getSCEV(Phi);
    const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(S);

    if (!AR || AR->getLoop() != &L)
      continue;

    InductionVariableInfo Info;
    Info.Phi = Phi;
    Info.InitialValue = AR->getStart();
    Info.Step = AR->getStepRecurrence(SE);

    if (PhiType->isPointerTy()) {
      Info.IsPointerInduction = true;
      Info.PointerElementType = PhiType->getPointerElementType();
      Info.ElementSize = DL.getTypeAllocSize(Info.PointerElementType);

      if (const SCEVConstant *StepConst = dyn_cast<SCEVConstant>(Info.Step)) {
        const APInt &StepVal = StepConst->getAPInt();
        Info.HasConstantStep = true;
        int64_t ByteStep = StepVal.getSExtValue();

        if (Info.ElementSize > 0) {
          Info.ConstantStep = ByteStep / Info.ElementSize;
          Info.IsIncreasing = Info.ConstantStep > 0;
          Info.IsDecreasing = Info.ConstantStep < 0;
        }
      }
    } else {
      if (const SCEVConstant *StepConst = dyn_cast<SCEVConstant>(Info.Step)) {
        const APInt &StepVal = StepConst->getAPInt();
        Info.HasConstantStep = true;
        Info.ConstantStep = StepVal.getSExtValue();
        Info.IsIncreasing = Info.ConstantStep > 0;
        Info.IsDecreasing = Info.ConstantStep < 0;
      }
    }

    InductionVars.push_back(Info);
  }
}

void InvariantCandidateGenerator::extractLoopBounds() {
  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);

  for (BasicBlock *ExitingBB : ExitingBlocks) {
    BranchInst *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    ICmpInst *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      continue;

    Value *Op0 = Cmp->getOperand(0);
    Value *Op1 = Cmp->getOperand(1);

    for (const auto &IVInfo : InductionVars) {
      if (Op0 == IVInfo.Phi || Op1 == IVInfo.Phi) {
        LoopBoundInfo BoundInfo;
        BoundInfo.ExitCond = Cmp;
        BoundInfo.Predicate = Cmp->getPredicate();
        BoundInfo.InductionVar = IVInfo.Phi;
        BoundInfo.BoundValue = (Op0 == IVInfo.Phi) ? Op1 : Op0;
        LoopBounds.push_back(BoundInfo);
      }
    }
  }
}

void InvariantCandidateGenerator::collectAssignments() {
  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      if (StoreInst *Store = dyn_cast<StoreInst>(&I)) {
        Value *Lval = Store->getPointerOperand();
        Value *Rval = Store->getValueOperand();

        if (Lval && Rval) {
          LoopAssignments.push_back(LoopAssignment(Lval, Rval));
        }
      } else if (LoadInst *Load = dyn_cast<LoadInst>(&I)) {
        Value *Lval = Load->getPointerOperand();
        Value *Rval = Load;

        if (Lval && Rval) {
          LoopAssignments.push_back(LoopAssignment(Lval, Rval));
        }
      }
    }
  }
}

void InvariantCandidateGenerator::analyzeValueDeltas() {
  for (const auto &Assign : LoopAssignments) {
    if (!Assign.Right)
      continue;

    const SCEV *RvalSCEV = SE.getSCEV(const_cast<Value *>(Assign.Right));
    if (isa<SCEVCouldNotCompute>(RvalSCEV))
      continue;

    ValueDelta Delta;
    Delta.Lval = Assign.Left;
    Delta.HasDelta = false;
    Delta.Delta = 0;

    if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(RvalSCEV)) {
      if (AR->getLoop() == &L) {
        const SCEV *Step = AR->getStepRecurrence(SE);
        if (const SCEVConstant *StepC = dyn_cast<SCEVConstant>(Step)) {
          const APInt &StepVal = StepC->getAPInt();
          if (StepVal.getBitWidth() <= 64) {
            Delta.HasDelta = true;
            Delta.Delta = StepVal.getSExtValue();
          }
        }
      }
    }

    ValueDeltas.push_back(Delta);
  }
}

void InvariantCandidateGenerator::inferTerminators() {
  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);

  for (BasicBlock *ExitingBB : ExitingBlocks) {
    BranchInst *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    ICmpInst *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      continue;

    Value *Op0 = Cmp->getOperand(0);
    Value *Op1 = Cmp->getOperand(1);

    int64_t CompareValue = 0;
    bool IsConstantCompare = false;

    if (const ConstantInt *CI = dyn_cast<ConstantInt>(Op1)) {
      CompareValue = CI->getSExtValue();
      IsConstantCompare = true;
    } else if (const ConstantInt *CI = dyn_cast<ConstantInt>(Op0)) {
      CompareValue = CI->getSExtValue();
      IsConstantCompare = true;
    }

    if (IsConstantCompare) {
      TerminatorInfo Info;
      Info.TerminateInt = CompareValue;

      if (isa<ConstantPointerNull>(IsConstantCompare ? Op1 : Op0)) {
        Info.Target = IsConstantCompare ? Op1 : Op0;
        Info.TerminateTest = nullptr;
        Terminators.push_back(Info);

        if (CompareValue != 0) {
          TerminatorInfo ZeroInfo;
          ZeroInfo.Target = Info.Target;
          ZeroInfo.TerminateTest = nullptr;
          ZeroInfo.TerminateInt = 0;
          Terminators.push_back(ZeroInfo);
        }
      } else {
        Info.Target = IsConstantCompare ? Op1 : Op0;
        Info.TerminateTest = Cmp;
        Terminators.push_back(Info);
      }
    }
  }
}

void InvariantCandidateGenerator::collectLoopComparisons() {
  for (BasicBlock *BB : L.blocks()) {
    for (Instruction &I : *BB) {
      if (ICmpInst *Cmp = dyn_cast<ICmpInst>(&I)) {
        Value *Op0 = Cmp->getOperand(0);
        Value *Op1 = Cmp->getOperand(1);

        LoopComparison Comp;
        Comp.Source = Op0;
        Comp.Target = Op1;
        Comp.Predicate = Cmp->getPredicate();
        Comp.IsPointerComparison = false;
        Comp.StrideType = nullptr;

        LoopComparisons.push_back(Comp);
      }
    }
  }
}

void InvariantCandidateGenerator::generateMonotonicityInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {

  for (const auto &IVInfo : InductionVars) {
    if (!IVInfo.HasConstantStep)
      continue;

    Z3Expr PhiExpr;
    Z3Expr InitExpr;

    if (IVInfo.IsPointerInduction) {
      PhiExpr = pointerToZ3Expr(IVInfo.Phi, IVInfo.PointerElementType);

      const SCEV *InitSCEV = IVInfo.InitialValue;
      if (const SCEVUnknown *InitUnknown = dyn_cast<SCEVUnknown>(InitSCEV)) {
        InitExpr =
            pointerToZ3Expr(InitUnknown->getValue(), IVInfo.PointerElementType);
      } else {
        InitExpr = scevToZ3Expr(InitSCEV);
      }
    } else {
      PhiExpr = valueToZ3Expr(IVInfo.Phi);
      InitExpr = scevToZ3Expr(IVInfo.InitialValue);
    }

    if (IVInfo.IsIncreasing) {
      InvariantCandidate Candidate(InvariantCandidate::MonotonicIncreasing);
      Candidate.Formula = PhiExpr >= InitExpr;

      if (IVInfo.IsPointerInduction) {
        Candidate.Description = "Pointer monotonically increases";
      } else {
        Candidate.Description = "Induction variable monotonically increases";
      }

      Candidate.InvolvedValues.push_back(IVInfo.Phi);
      Candidates.push_back(Candidate);
    }

    if (IVInfo.IsDecreasing) {
      InvariantCandidate Candidate(InvariantCandidate::MonotonicDecreasing);
      Candidate.Formula = PhiExpr <= InitExpr;

      if (IVInfo.IsPointerInduction) {
        Candidate.Description = "Pointer monotonically decreases";
      } else {
        Candidate.Description = "Induction variable monotonically decreases";
      }

      Candidate.InvolvedValues.push_back(IVInfo.Phi);
      Candidates.push_back(Candidate);
    }
  }
}

void InvariantCandidateGenerator::generateBoundInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {

  for (const auto &BoundInfo : LoopBounds) {
    const InductionVariableInfo *IVInfo = nullptr;
    for (const auto &IV : InductionVars) {
      if (IV.Phi == BoundInfo.InductionVar) {
        IVInfo = &IV;
        break;
      }
    }

    if (!IVInfo)
      continue;

    Z3Expr IVExpr;
    Z3Expr BoundExpr;

    if (IVInfo->IsPointerInduction) {
      IVExpr =
          pointerToZ3Expr(BoundInfo.InductionVar, IVInfo->PointerElementType);

      if (BoundInfo.BoundValue->getType()->isPointerTy()) {
        BoundExpr =
            pointerToZ3Expr(BoundInfo.BoundValue, IVInfo->PointerElementType);
      } else {
        BoundExpr = valueToZ3Expr(BoundInfo.BoundValue);
      }
    } else {
      IVExpr = valueToZ3Expr(BoundInfo.InductionVar);
      BoundExpr = valueToZ3Expr(BoundInfo.BoundValue);
    }

    const bool HasStep = IVInfo->HasConstantStep;
    const int64_t Step = IVInfo->ConstantStep;
    const int64_t AbsStep = Step >= 0 ? Step : -Step;

    auto mkConst = [](int64_t V) { return Z3Expr(V); };

    switch (BoundInfo.Predicate) {
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_ULT:
      if (HasStep && Step > 0 && AbsStep > 0) {
        InvariantCandidate Candidate(InvariantCandidate::UpperBound);
        Candidate.Formula = IVExpr <= (BoundExpr + mkConst(AbsStep - 1));
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer upper bound (adjusted from <)"
                : "Induction variable upper bound (adjusted from <)";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      } else {
        InvariantCandidate Candidate(InvariantCandidate::UpperBound);
        Candidate.Formula = IVExpr < BoundExpr;
        Candidate.Description = IVInfo->IsPointerInduction
                                    ? "Pointer less than bound"
                                    : "Induction variable less than bound";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      }
      break;
    case CmpInst::ICMP_SLE:
    case CmpInst::ICMP_ULE:
      if (HasStep && Step > 0 && AbsStep > 0) {
        InvariantCandidate Candidate(InvariantCandidate::UpperBound);
        Candidate.Formula = IVExpr <= (BoundExpr + mkConst(AbsStep));
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer upper bound (adjusted from <=)"
                : "Induction variable upper bound (adjusted from <=)";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      } else {
        InvariantCandidate Candidate(InvariantCandidate::UpperBound);
        Candidate.Formula = IVExpr <= BoundExpr;
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer less than or equal to bound"
                : "Induction variable less than or equal to bound";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      }
      break;
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_UGT:
      if (HasStep && Step < 0 && AbsStep > 0) {
        InvariantCandidate Candidate(InvariantCandidate::LowerBound);
        Candidate.Formula = IVExpr >= (BoundExpr - mkConst(AbsStep - 1));
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer lower bound (adjusted from >)"
                : "Induction variable lower bound (adjusted from >)";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      } else {
        InvariantCandidate Candidate(InvariantCandidate::LowerBound);
        Candidate.Formula = IVExpr > BoundExpr;
        Candidate.Description = IVInfo->IsPointerInduction
                                    ? "Pointer greater than bound"
                                    : "Induction variable greater than bound";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      }
      break;
    case CmpInst::ICMP_SGE:
    case CmpInst::ICMP_UGE:
      if (HasStep && Step < 0 && AbsStep > 0) {
        InvariantCandidate Candidate(InvariantCandidate::LowerBound);
        Candidate.Formula = IVExpr >= (BoundExpr - mkConst(AbsStep));
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer lower bound (adjusted from >=)"
                : "Induction variable lower bound (adjusted from >=)";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      } else {
        InvariantCandidate Candidate(InvariantCandidate::LowerBound);
        Candidate.Formula = IVExpr >= BoundExpr;
        Candidate.Description =
            IVInfo->IsPointerInduction
                ? "Pointer greater than or equal to bound"
                : "Induction variable greater than or equal to bound";
        Candidate.InvolvedValues.push_back(BoundInfo.InductionVar);
        Candidate.InvolvedValues.push_back(BoundInfo.BoundValue);
        Candidates.push_back(Candidate);
      }
      break;
    default:
      continue;
    }
  }
}

void InvariantCandidateGenerator::generateLinearRelationInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {

  for (size_t i = 0; i < InductionVars.size(); ++i) {
    for (size_t j = i + 1; j < InductionVars.size(); ++j) {
      const auto &IV1 = InductionVars[i];
      const auto &IV2 = InductionVars[j];

      if (!IV1.HasConstantStep || !IV2.HasConstantStep)
        continue;

      if (IV1.ConstantStep == 0 || IV2.ConstantStep == 0)
        continue;

      Z3Expr Phi1 = valueToZ3Expr(IV1.Phi);
      Z3Expr Phi2 = valueToZ3Expr(IV2.Phi);
      Z3Expr Init1 = scevToZ3Expr(IV1.InitialValue);
      Z3Expr Init2 = scevToZ3Expr(IV2.InitialValue);

      Z3Expr Diff1 =
          (Phi1 - Init1) * Z3Expr(static_cast<int>(IV2.ConstantStep));
      Z3Expr Diff2 =
          (Phi2 - Init2) * Z3Expr(static_cast<int>(IV1.ConstantStep));

      InvariantCandidate Candidate(InvariantCandidate::LinearRelationship);
      Candidate.Formula = Diff1 == Diff2;
      Candidate.Description = "Linear relationship between induction variables";
      Candidate.InvolvedValues.push_back(IV1.Phi);
      Candidate.InvolvedValues.push_back(IV2.Phi);
      Candidates.push_back(Candidate);
    }
  }
}

void InvariantCandidateGenerator::generateAssignmentBasedInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {
  for (const auto &Assign : LoopAssignments) {
    if (!isa<LoadInst>(Assign.Right))
      continue;

    for (const auto &Comp : LoopComparisons) {
      if (Comp.Source != Assign.Right)
        continue;

      Z3Expr LeftExpr = valueToZ3Expr(Assign.Left);
      Z3Expr TargetExpr = valueToZ3Expr(Comp.Target);

      InvariantCandidate Candidate(InvariantCandidate::AssignmentBased);

      switch (Comp.Predicate) {
      case CmpInst::ICMP_EQ:
        Candidate.Formula = LeftExpr == TargetExpr;
        Candidate.Description = "Assignment-based equality invariant";
        break;
      case CmpInst::ICMP_NE:
        Candidate.Formula = LeftExpr != TargetExpr;
        Candidate.Description = "Assignment-based inequality invariant";
        break;
      case CmpInst::ICMP_SLT:
      case CmpInst::ICMP_ULT:
        Candidate.Formula = LeftExpr < TargetExpr;
        Candidate.Description = "Assignment-based less-than invariant";
        break;
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_ULE:
        Candidate.Formula = LeftExpr <= TargetExpr;
        Candidate.Description = "Assignment-based less-or-equal invariant";
        break;
      case CmpInst::ICMP_SGT:
      case CmpInst::ICMP_UGT:
        Candidate.Formula = LeftExpr > TargetExpr;
        Candidate.Description = "Assignment-based greater-than invariant";
        break;
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_UGE:
        Candidate.Formula = LeftExpr >= TargetExpr;
        Candidate.Description = "Assignment-based greater-or-equal invariant";
        break;
      default:
        continue;
      }

      Candidate.InvolvedValues.push_back(Assign.Left);
      Candidate.InvolvedValues.push_back(Comp.Target);
      Candidates.push_back(Candidate);

      tryImplicationWeakening(Candidates, Candidate);
    }
  }
}

void InvariantCandidateGenerator::generateTerminatorInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {
  for (const auto &Term : Terminators) {
    if (!Term.Target)
      continue;

    Z3Expr TargetExpr = valueToZ3Expr(Term.Target);
    z3::context &Ctx = Z3Expr::getContext();
    Z3Expr ZeroExpr(Ctx.int_val(0));

    InvariantCandidate Candidate(InvariantCandidate::Terminator);

    if (Term.TerminateInt == 0) {
      Candidate.Formula = TargetExpr != ZeroExpr;
      Candidate.Description = "Terminator: target != 0";
    } else {
      Z3Expr BoundExpr(Ctx.int_val(Term.TerminateInt));
      Candidate.Formula = TargetExpr != BoundExpr;
      Candidate.Description =
          "Terminator: target != " + std::to_string(Term.TerminateInt);
    }

    Candidate.InvolvedValues.push_back(Term.Target);
    Candidates.push_back(Candidate);
  }
}

void InvariantCandidateGenerator::generateFlagBasedInvariants(
    SmallVectorImpl<InvariantCandidate> &Candidates) {
  for (const auto &Assign : LoopAssignments) {
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(Assign.Right)) {
      int64_t FlagValue = CI->getSExtValue();

      for (const auto &Comp : LoopComparisons) {
        if (Comp.Source != Assign.Left)
          continue;

        if (const ConstantInt *CompCI = dyn_cast<ConstantInt>(Comp.Target)) {
          if (CompCI->getSExtValue() == FlagValue)
            continue;

          if (Comp.Predicate == CmpInst::ICMP_EQ ||
              Comp.Predicate == CmpInst::ICMP_NE) {
            Z3Expr InitLval = getImplicationPremise(Assign.Left);
            Z3Expr Lval = valueToZ3Expr(Assign.Left);
            z3::context &Ctx = Z3Expr::getContext();
            Z3Expr InitValue(Ctx.int_val(FlagValue));
            Z3Expr CompValue = valueToZ3Expr(Comp.Target);

            Z3Expr InitCheck = (InitLval == InitValue);
            Z3Expr CurrentCheck = (Lval == CompValue);
            Z3Expr NotEqualCheck = (Lval != InitValue);

            InvariantCandidate Candidate(InvariantCandidate::FlagBased);
            Candidate.IsImplication = true;
            Candidate.Formula = (!NotEqualCheck) || CurrentCheck;
            Candidate.Premise = InitCheck;
            Candidate.Description = "Flag-based conditional invariant";
            Candidate.InvolvedValues.push_back(Assign.Left);
            Candidate.InvolvedValues.push_back(Comp.Target);
            Candidates.push_back(Candidate);
          }
        }
      }
    }
  }
}

void InvariantCandidateGenerator::tryImplicationWeakening(
    SmallVectorImpl<InvariantCandidate> &Candidates,
    const InvariantCandidate &FailedCandidate) {
  for (const auto &IVInfo : InductionVars) {
    if (FailedCandidate.InvolvedValues.empty())
      continue;

    const Value *FirstVal = FailedCandidate.InvolvedValues[0];

    if (FirstVal != IVInfo.Phi)
      continue;

    Z3Expr InitExpr = scevToZ3Expr(IVInfo.InitialValue);
    Z3Expr CurrentExpr = valueToZ3Expr(IVInfo.Phi);

    InvariantCandidate ImplicationCandidate(InvariantCandidate::Implication);
    ImplicationCandidate.IsImplication = true;
    ImplicationCandidate.Premise = (CurrentExpr >= InitExpr);
    ImplicationCandidate.Formula =
        (!ImplicationCandidate.Premise) || FailedCandidate.Formula;
    ImplicationCandidate.Description =
        "Weakened invariant with monotonicity premise";
    ImplicationCandidate.InvolvedValues = FailedCandidate.InvolvedValues;
    Candidates.push_back(ImplicationCandidate);
    break;
  }
}

Z3Expr InvariantCandidateGenerator::scevToZ3Expr(const SCEV *S) {
  if (!S)
    return Z3Expr(0);

  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(S)) {
    const APInt &Val = SC->getAPInt();
    if (Val.getBitWidth() <= 64) {
      return Z3Expr(Val.getSExtValue());
    }

    llvm::SmallString<64> Tmp;
    Val.toStringSigned(Tmp);
    return Z3Expr(Z3Expr::getContext().int_val(Tmp.c_str()));
  }

  if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(S)) {
    return valueToZ3Expr(SU->getValue());
  }

  if (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(S)) {
    Z3Expr Acc(0);
    for (const SCEV *Op : Add->operands())
      Acc = Acc + scevToZ3Expr(Op);
    return Acc;
  }

  if (const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(S)) {
    bool First = true;
    Z3Expr Acc(1);
    for (const SCEV *Op : Mul->operands()) {
      if (First) {
        Acc = scevToZ3Expr(Op);
        First = false;
      } else {
        Acc = Acc * scevToZ3Expr(Op);
      }
    }
    return Acc;
  }

  if (const SCEVUDivExpr *Div = dyn_cast<SCEVUDivExpr>(S)) {
    return scevToZ3Expr(Div->getLHS()) / scevToZ3Expr(Div->getRHS());
  }

  if (const SCEVSignExtendExpr *Ext = dyn_cast<SCEVSignExtendExpr>(S)) {
    return scevToZ3Expr(Ext->getOperand());
  }
  if (const SCEVZeroExtendExpr *Ext = dyn_cast<SCEVZeroExtendExpr>(S)) {
    return scevToZ3Expr(Ext->getOperand());
  }
  if (const SCEVTruncateExpr *Tr = dyn_cast<SCEVTruncateExpr>(S)) {
    return scevToZ3Expr(Tr->getOperand());
  }

  // Conservative fallback: treat as an uninterpreted integer (stable per SCEV).
  std::string Name =
      "scev_" + std::to_string(reinterpret_cast<uintptr_t>(S));
  return Z3Expr(Z3Expr::getContext().int_const(Name.c_str()));
}

Z3Expr InvariantCandidateGenerator::valueToZ3Expr(const Value *V) {
  if (!V)
    return Z3Expr(0);

  if (const ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
    const APInt &Val = CI->getValue();
    if (Val.getBitWidth() <= 64)
      return Z3Expr(Val.getSExtValue());
    llvm::SmallString<64> Tmp;
    Val.toStringSigned(Tmp);
    return Z3Expr(Z3Expr::getContext().int_val(Tmp.c_str()));
  }

  if (isa<ConstantPointerNull>(V))
    return Z3Expr(0);

  std::string VarName = z3NameForValue(V);
  z3::context &Ctx = Z3Expr::getContext();
  return Z3Expr(Ctx.int_const(VarName.c_str()));
}

Z3Expr InvariantCandidateGenerator::pointerToZ3Expr(const Value *V,
                                                    Type *ElementType) {
  std::string VarName = V->hasName() ? V->getName().str() : "ptr";
  z3::context &Ctx = Z3Expr::getContext();

  Module *M = L.getHeader()->getParent()->getParent();
  const DataLayout &DL = M->getDataLayout();
  uint64_t ElemSize = DL.getTypeAllocSize(ElementType);

  if (ElemSize == 0)
    ElemSize = 1;

  z3::expr PtrExpr = Ctx.int_const(VarName.c_str());

  return Z3Expr(PtrExpr);
}

Z3Expr InvariantCandidateGenerator::getInitialValue(const Value *V) {
  if (!V)
    return Z3Expr(0);

  for (const auto &IVInfo : InductionVars) {
    if (IVInfo.Phi == V) {
      return scevToZ3Expr(IVInfo.InitialValue);
    }
  }

  if (const PHINode *Phi = dyn_cast<PHINode>(V)) {
    if (Phi->getParent() == L.getHeader()) {
      const Value *UniqueIncoming = nullptr;
      for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
        const BasicBlock *IncomingBB = Phi->getIncomingBlock(I);
        if (L.contains(IncomingBB))
          continue;
        const Value *IncomingV = Phi->getIncomingValue(I);
        if (!UniqueIncoming) {
          UniqueIncoming = IncomingV;
        } else if (UniqueIncoming != IncomingV) {
          UniqueIncoming = nullptr;
          break;
        }
      }
      if (UniqueIncoming)
        return valueToZ3Expr(UniqueIncoming);
    }
  }

  return valueToZ3Expr(V);
}

Z3Expr InvariantCandidateGenerator::getImplicationPremise(const Value *V) {
  for (const auto &IVInfo : InductionVars) {
    if (IVInfo.Phi == V && IVInfo.HasConstantStep) {
      return scevToZ3Expr(IVInfo.InitialValue);
    }
  }

  return getInitialValue(V);
}
