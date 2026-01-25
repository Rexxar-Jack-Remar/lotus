#include "Verification/LoopInvariants/InvariantProver.h"

#include "Verification/LoopInvariants/Z3ValueNaming.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "z3++.h"

#include <cstdint>
#include <unordered_set>

using namespace llvm;
using namespace lotus;

static z3::expr translateValue(z3::context &Ctx, const Loop &L,
                               const Value *V) {
  if (!V)
    return Ctx.int_val(0);

  if (const ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
    const APInt &Val = CI->getValue();
    if (Val.getBitWidth() <= 64)
      return Ctx.int_val(Val.getSExtValue());
    llvm::SmallString<64> Tmp;
    Val.toStringSigned(Tmp);
    return Ctx.int_val(Tmp.c_str());
  }

  if (isa<ConstantPointerNull>(V))
    return Ctx.int_val(0);

  if (const CastInst *Cast = dyn_cast<CastInst>(V))
    return translateValue(Ctx, L, Cast->getOperand(0));

  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(V)) {
    z3::expr E0 = translateValue(Ctx, L, BO->getOperand(0));
    z3::expr E1 = translateValue(Ctx, L, BO->getOperand(1));
    switch (BO->getOpcode()) {
    case Instruction::Add:
      return E0 + E1;
    case Instruction::Sub:
      return E0 - E1;
    case Instruction::Mul:
      return E0 * E1;
    default:
      break;
    }
  }

  // If the value is loop-invariant, it is safe to treat it as a constant
  // across iterations.
  return Ctx.int_const(lotus::z3NameForValue(V).c_str());
}

static llvm::Optional<z3::expr> translateICmp(z3::context &Ctx, const Loop &L,
                                             const ICmpInst *Cmp) {
  if (!Cmp)
    return llvm::None;

  z3::expr LHS = translateValue(Ctx, L, Cmp->getOperand(0));
  z3::expr RHS = translateValue(Ctx, L, Cmp->getOperand(1));

  switch (Cmp->getPredicate()) {
  case CmpInst::ICMP_EQ:
    return LHS == RHS;
  case CmpInst::ICMP_NE:
    return LHS != RHS;
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_ULT:
    return LHS < RHS;
  case CmpInst::ICMP_SLE:
  case CmpInst::ICMP_ULE:
    return LHS <= RHS;
  case CmpInst::ICMP_SGT:
  case CmpInst::ICMP_UGT:
    return LHS > RHS;
  case CmpInst::ICMP_SGE:
  case CmpInst::ICMP_UGE:
    return LHS >= RHS;
  default:
    return llvm::None;
  }
}

std::string InvariantProver::getValueName(const Value *V) {
  return z3NameForValue(V);
}

InvariantProver::InvariantProver(const Loop &Loop, ScalarEvolution &SE,
                                 DominatorTree &DT)
    : L(Loop), SE(SE), DT(DT), Ctx(&Z3Expr::getContext()) {}

InvariantProver::~InvariantProver() = default;

z3::expr InvariantProver::getInitialValue(const PHINode *Phi) {
  if (!Phi) {
    return Ctx->int_val(0);
  }

  const Value *Incoming = nullptr;

  if (BasicBlock *Preheader = L.getLoopPreheader()) {
    Incoming = Phi->getIncomingValueForBlock(Preheader);
  } else {
    // Try to find a unique incoming value from outside the loop.
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
      const BasicBlock *IncomingBB = Phi->getIncomingBlock(I);
      if (L.contains(IncomingBB))
        continue;
      const Value *V = Phi->getIncomingValue(I);
      if (!Incoming) {
        Incoming = V;
      } else if (Incoming != V) {
        Incoming = nullptr;
        break;
      }
    }
  }

  if (Incoming) {
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(Incoming)) {
      const APInt &Val = CI->getValue();
      if (Val.getBitWidth() <= 64)
        return Ctx->int_val(Val.getSExtValue());
      llvm::SmallString<64> Tmp;
      Val.toStringSigned(Tmp);
      return Ctx->int_val(Tmp.c_str());
    }
    const SCEV *S = SE.getSCEV(const_cast<Value *>(Incoming));
    if (!isa<SCEVCouldNotCompute>(S)) {
      return scevToZ3Expr(S);
    }
  }

  // Fall back to a symbolic initial value.
  std::string PhiName = getValueName(Phi);
  return Ctx->int_const((PhiName + "_init").c_str());
}

z3::expr InvariantProver::getStepExpr(const PHINode *Phi) {
  const SCEV *PhiSCEV = SE.getSCEV(const_cast<PHINode *>(Phi));
  if (isa<SCEVCouldNotCompute>(PhiSCEV))
    return Ctx->int_const((getValueName(Phi) + "_step").c_str());

  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiSCEV);
  if (!AR || AR->getLoop() != &L)
    return Ctx->int_const((getValueName(Phi) + "_step").c_str());

  const SCEV *Step = AR->getStepRecurrence(SE);
  if (!isa<SCEVCouldNotCompute>(Step))
    return scevToZ3Expr(Step);
  return Ctx->int_const((getValueName(Phi) + "_step").c_str());
}

z3::expr InvariantProver::scevToZ3Expr(const SCEV *S) {
  if (!S)
    return Ctx->int_val(0);

  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(S)) {
    const APInt &Val = SC->getAPInt();
    if (Val.getBitWidth() <= 64)
      return Ctx->int_val(Val.getSExtValue());
    llvm::SmallString<64> Tmp;
    Val.toStringSigned(Tmp);
    return Ctx->int_val(Tmp.c_str());
  }

  if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(S)) {
    const Value *V = SU->getValue();
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
      const APInt &Val = CI->getValue();
      if (Val.getBitWidth() <= 64)
        return Ctx->int_val(Val.getSExtValue());
      llvm::SmallString<64> Tmp;
      Val.toStringSigned(Tmp);
      return Ctx->int_val(Tmp.c_str());
    }
    return Ctx->int_const(getValueName(V).c_str());
  }

  if (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(S)) {
    z3::expr Acc = Ctx->int_val(0);
    for (const SCEV *Op : Add->operands())
      Acc = Acc + scevToZ3Expr(Op);
    return Acc;
  }

  if (const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(S)) {
    bool First = true;
    z3::expr Acc = Ctx->int_val(1);
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

  if (const SCEVSignExtendExpr *Ext = dyn_cast<SCEVSignExtendExpr>(S))
    return scevToZ3Expr(Ext->getOperand());
  if (const SCEVZeroExtendExpr *Ext = dyn_cast<SCEVZeroExtendExpr>(S))
    return scevToZ3Expr(Ext->getOperand());
  if (const SCEVTruncateExpr *Tr = dyn_cast<SCEVTruncateExpr>(S))
    return scevToZ3Expr(Tr->getOperand());

  std::string Name = "scev_" + std::to_string(reinterpret_cast<uintptr_t>(S));
  return Ctx->int_const(Name.c_str());
}

z3::expr InvariantProver::renameForNextState(const InvariantCandidate &Candidate,
                                             const z3::expr &Invariant) {
  z3::expr_vector From(*Ctx);
  z3::expr_vector To(*Ctx);

  std::unordered_set<std::string> Seen;
  for (const Value *V : Candidate.InvolvedValues) {
    if (!V || isa<Constant>(V))
      continue;

    if (L.isLoopInvariant(V))
      continue;

    std::string Name = getValueName(V);
    if (!Seen.insert(Name).second)
      continue;

    From.push_back(Ctx->int_const(Name.c_str()));
    To.push_back(Ctx->int_const((Name + "_next").c_str()));
  }

  if (From.size() == 0)
    return Invariant;

  // Make a non-const copy since substitute is not a const method
  z3::expr NonConstInvariant = Invariant;
  return NonConstInvariant.substitute(From, To);
}

void InvariantProver::buildBaseCaseConstraints(z3::solver &Solver) {
  BasicBlock *Header = L.getHeader();

  for (auto &Inst : *Header) {
    PHINode *Phi = dyn_cast<PHINode>(&Inst);
    if (!Phi)
      break;

    const SCEV *PhiSCEV = SE.getSCEV(Phi);
    if (isa<SCEVCouldNotCompute>(PhiSCEV))
      continue;

    const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiSCEV);
    if (!AR || AR->getLoop() != &L)
      continue;

    std::string PhiName = getValueName(Phi);
    z3::expr PhiVar = Ctx->int_const(PhiName.c_str());
    z3::expr InitVal = getInitialValue(Phi);

    Solver.add(PhiVar == InitVal);
  }
}

void InvariantProver::buildStepCaseConstraints(z3::solver &Solver) {
  BasicBlock *Header = L.getHeader();

  for (auto &Inst : *Header) {
    PHINode *Phi = dyn_cast<PHINode>(&Inst);
    if (!Phi)
      break;

    const SCEV *PhiSCEV = SE.getSCEV(Phi);
    if (isa<SCEVCouldNotCompute>(PhiSCEV))
      continue;

    const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiSCEV);
    if (!AR || AR->getLoop() != &L)
      continue;

    std::string PhiName = getValueName(Phi);
    z3::expr PhiVar = Ctx->int_const(PhiName.c_str());
    z3::expr PhiNextVar = Ctx->int_const((PhiName + "_next").c_str());
    z3::expr StepExpr = getStepExpr(Phi);

    Solver.add(PhiNextVar == (PhiVar + StepExpr));
  }
}

InvariantProver::ProofResult
InvariantProver::proveInvariant(const InvariantCandidate &Candidate) {
  if (Candidate.Formula.id() == 0)
    return ProofResult(false, "Invalid formula");

  z3::solver Solver(*Ctx);

  z3::expr InvariantToProve = Candidate.Formula.getExpr();

  if (Candidate.IsImplication && Candidate.Premise.id() != 0) {
    z3::expr Premise = Candidate.Premise.getExpr();
    z3::expr Conclusion = Candidate.Formula.getExpr();
    InvariantToProve = (!Premise) || Conclusion;
    llvm::errs() << "DEBUG: Proving implication: (!premise) || conclusion\n";
  }

  ProofResult BaseResult = proveBase(InvariantToProve, Solver);
  if (!BaseResult.IsProven)
    return BaseResult;

  Solver.reset();

  ProofResult StepResult = proveStep(renameForNextState(Candidate, InvariantToProve),
                                     InvariantToProve, Solver);
  return StepResult;
}

InvariantProver::ProofResult
InvariantProver::proveBase(const z3::expr &Invariant, z3::solver &Solver) {
  Solver.push();

  buildBaseCaseConstraints(Solver);

  z3::expr NotInv = !Invariant;
  Solver.add(NotInv);

  z3::check_result Result = Solver.check();
  llvm::errs() << "DEBUG: Base case solver result: ";
  if (Result == z3::unsat) {
    llvm::errs() << "UNSAT\n";
  } else if (Result == z3::sat) {
    llvm::errs() << "SAT (invariant can be violated)\n";
    z3::model Model = Solver.get_model();
    llvm::errs() << "DEBUG: Model: " << Model << "\n";
  } else {
    llvm::errs() << "UNKNOWN\n";
  }

  Solver.pop();

  if (Result == z3::unsat) {
    return ProofResult(true);
  } else if (Result == z3::sat) {
    return ProofResult(false,
                       "Base case fails: invariant does not hold at entry");
  } else {
    return ProofResult(false, "Base case unknown");
  }
}

InvariantProver::ProofResult
InvariantProver::proveStep(const z3::expr &InvariantNext,
                           const z3::expr &InvariantCurrent,
                           z3::solver &Solver) {
  SmallVector<z3::expr, 4> BackedgeGuards;
  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);
  for (BasicBlock *ExitingBB : ExitingBlocks) {
    BranchInst *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    ICmpInst *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      continue;

    const bool TrueInLoop = L.contains(BI->getSuccessor(0));
    const bool FalseInLoop = L.contains(BI->getSuccessor(1));
    if (TrueInLoop == FalseInLoop)
      continue;

    auto Cond = translateICmp(*Ctx, L, Cmp);
    if (!Cond)
      continue;

    BackedgeGuards.push_back(TrueInLoop ? *Cond : !(*Cond));
  }

  // If we can't derive any guard, fall back to an unguarded step check (more
  // conservative; may fail to prove some invariants).
  if (BackedgeGuards.empty())
    BackedgeGuards.push_back(Ctx->bool_val(true));

  for (const z3::expr &Guard : BackedgeGuards) {
    Solver.push();

    Solver.add(InvariantCurrent);
    Solver.add(Guard);
    buildStepCaseConstraints(Solver);

    z3::expr NotInv = !InvariantNext;
    Solver.add(NotInv);

    z3::check_result Result = Solver.check();
    Solver.pop();

    if (Result == z3::sat)
      return ProofResult(false, "Step case fails: invariant not preserved");
    if (Result == z3::unknown)
      return ProofResult(false, "Step case unknown");
  }

  return ProofResult(true);
}
