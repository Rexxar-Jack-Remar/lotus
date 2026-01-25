#include "Verification/LoopInvariants/FunctionInvariantCandidateGenerator.h"
#include "Verification/LoopInvariants/Z3ValueNaming.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
//#include "llvm/Support/raw_ostream.h"

#include <cstdint>

using namespace llvm;
using namespace lotus;

FunctionInvariantCandidateGenerator::FunctionInvariantCandidateGenerator(
    const Function &F, ScalarEvolution &SE)
    : Func(F), SE(SE) {}

static Z3Expr returnZ3Expr() {
  z3::context &Ctx = Z3Expr::getContext();
  return Z3Expr(Ctx.int_const("ret"));
}

void FunctionInvariantCandidateGenerator::generateCandidates(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {

  collectReturnValues();

  generateReturnBoundInvariants(Candidates);
  generateReturnEqualityInvariants(Candidates);
  generateReturnNonNegativeInvariants(Candidates);
  generateReturnComparisonInvariants(Candidates);
  generateReturnPlusComponentInvariants(Candidates);
  generateReturnMinusNonNegativeInvariants(Candidates);
}

void FunctionInvariantCandidateGenerator::collectReturnValues() {
  for (const BasicBlock &BB : Func) {
    for (const Instruction &I : BB) {
      if (const ReturnInst *RI = dyn_cast<ReturnInst>(&I)) {
        ReturnInsts.push_back(RI);

        if (Value *RetVal = RI->getReturnValue()) {
          ReturnValues.push_back(RetVal);
        }
      }
    }
  }
}

void FunctionInvariantCandidateGenerator::generateReturnBoundInvariants(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    const SCEV *RetSCEV = SE.getSCEV(const_cast<Value *>(RetVal));
    if (isa<SCEVCouldNotCompute>(RetSCEV))
      continue;

    if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(RetSCEV)) {
      const APInt &Val = SC->getAPInt();
      if (Val.getBitWidth() <= 64 && Val.isNonNegative()) {
        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr ZeroExpr = Z3Expr(0);

        FunctionInvariantCandidate Candidate(
            FunctionInvariantCandidate::ReturnBound);
        Candidate.Formula = RetExpr >= ZeroExpr;
        Candidate.Description = "Return value is non-negative";
        Candidate.InvolvedValues.push_back(RetVal);
        Candidates.push_back(Candidate);
      }
    }
  }
}

void FunctionInvariantCandidateGenerator::generateReturnNonNegativeInvariants(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(RetVal)) {
      unsigned OpCode = BO->getOpcode();

      if (OpCode == Instruction::Add) {
        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr ZeroExpr = Z3Expr(0);

        FunctionInvariantCandidate Candidate(
            FunctionInvariantCandidate::ReturnNonNegative);
        Candidate.Formula = RetExpr >= ZeroExpr;
        Candidate.Description = "Addition result is non-negative";
        Candidate.InvolvedValues.push_back(RetVal);
        Candidates.push_back(Candidate);
      }

      if (OpCode == Instruction::Sub) {
        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr ZeroExpr = Z3Expr(0);

        FunctionInvariantCandidate Candidate(
            FunctionInvariantCandidate::ReturnNonNegative);
        Candidate.Formula = RetExpr >= ZeroExpr;
        Candidate.Description = "Subtraction result is non-negative";
        Candidate.InvolvedValues.push_back(RetVal);
        Candidates.push_back(Candidate);
      }
    }
  }
}

void FunctionInvariantCandidateGenerator::generateReturnEqualityInvariants(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(RetVal)) {
      Z3Expr RetExpr = returnZ3Expr();
      Z3Expr ConstExpr = valueToZ3Expr(CI);
      FunctionInvariantCandidate Candidate(
          FunctionInvariantCandidate::ReturnEquality);
      Candidate.Formula = RetExpr == ConstExpr;
      Candidate.Description = "Return value equals constant";
      Candidate.InvolvedValues.push_back(RetVal);
      Candidates.push_back(Candidate);
      continue;
    }

    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(RetVal)) {
      unsigned OpCode = BO->getOpcode();
      if (OpCode != Instruction::Add && OpCode != Instruction::Sub)
        continue;

      Value *Op0 = BO->getOperand(0);
      Value *Op1 = BO->getOperand(1);

      Z3Expr RetExpr = returnZ3Expr();
      Z3Expr Op0Expr = valueToZ3Expr(Op0);
      Z3Expr Op1Expr = valueToZ3Expr(Op1);

      FunctionInvariantCandidate Candidate(
          FunctionInvariantCandidate::ReturnEquality);
      Candidate.Formula =
          (OpCode == Instruction::Add) ? (RetExpr == (Op0Expr + Op1Expr))
                                       : (RetExpr == (Op0Expr - Op1Expr));
      Candidate.Description = (OpCode == Instruction::Add)
                                  ? "Return value equals sum of operands"
                                  : "Return value equals difference of operands";
      Candidate.InvolvedValues.push_back(RetVal);
      Candidate.InvolvedValues.push_back(Op0);
      Candidate.InvolvedValues.push_back(Op1);
      Candidates.push_back(Candidate);
    }
  }
}

void FunctionInvariantCandidateGenerator::generateReturnComparisonInvariants(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(RetVal)) {
      unsigned OpCode = BO->getOpcode();

      if (OpCode == Instruction::Sub) {
        Value *Op0 = BO->getOperand(0);
        Value *Op1 = BO->getOperand(1);

        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr Op0Expr = valueToZ3Expr(Op0);
        Z3Expr Op1Expr = valueToZ3Expr(Op1);

        FunctionInvariantCandidate Candidate(
            FunctionInvariantCandidate::ReturnComparison);

        Candidate.Formula = RetExpr >= Op1Expr;
        Candidate.Description = "Return value >= second operand of subtraction";
        Candidate.InvolvedValues.push_back(RetVal);
        Candidate.InvolvedValues.push_back(Op1);
        Candidates.push_back(Candidate);
      }
    }
  }
}

void FunctionInvariantCandidateGenerator::generateReturnPlusComponentInvariants(
    SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(RetVal)) {
      unsigned OpCode = BO->getOpcode();

      if (OpCode == Instruction::Add) {
        Value *Op0 = BO->getOperand(0);
        Value *Op1 = BO->getOperand(1);

        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr Op0Expr = valueToZ3Expr(Op0);
        Z3Expr Op1Expr = valueToZ3Expr(Op1);

        {
          FunctionInvariantCandidate Candidate(
              FunctionInvariantCandidate::ReturnPlusComponents);
          Candidate.Formula = RetExpr >= Op0Expr;
          Candidate.Description = "Return value >= first operand of addition";
          Candidate.InvolvedValues.push_back(RetVal);
          Candidate.InvolvedValues.push_back(Op0);
          Candidates.push_back(Candidate);
        }

        {
          FunctionInvariantCandidate Candidate(
              FunctionInvariantCandidate::ReturnPlusComponents);
          Candidate.Formula = RetExpr >= Op1Expr;
          Candidate.Description = "Return value >= second operand of addition";
          Candidate.InvolvedValues.push_back(RetVal);
          Candidate.InvolvedValues.push_back(Op1);
          Candidates.push_back(Candidate);
        }
      }
    }
  }
}

void FunctionInvariantCandidateGenerator::
    generateReturnMinusNonNegativeInvariants(
        SmallVectorImpl<FunctionInvariantCandidate> &Candidates) {
  for (const Value *RetVal : ReturnValues) {
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(RetVal)) {
      unsigned OpCode = BO->getOpcode();

      if (OpCode == Instruction::Sub) {
        Z3Expr RetExpr = returnZ3Expr();
        Z3Expr ZeroExpr = Z3Expr(0);

        FunctionInvariantCandidate Candidate(
            FunctionInvariantCandidate::ReturnMinusNonNegative);
        Candidate.Formula = RetExpr >= ZeroExpr;
        Candidate.Description = "Return value from subtraction is non-negative";
        Candidate.InvolvedValues.push_back(RetVal);
        Candidates.push_back(Candidate);
      }
    }
  }
}

Z3Expr FunctionInvariantCandidateGenerator::valueToZ3Expr(const Value *V) {
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

Z3Expr FunctionInvariantCandidateGenerator::scevToZ3Expr(const SCEV *S) {
  if (!S)
    return Z3Expr(0);

  if (const SCEVConstant *SC = dyn_cast<SCEVConstant>(S)) {
    const APInt &Val = SC->getAPInt();
    if (Val.getBitWidth() <= 64)
      return Z3Expr(Val.getSExtValue());
    llvm::SmallString<64> Tmp;
    Val.toStringSigned(Tmp);
    return Z3Expr(Z3Expr::getContext().int_val(Tmp.c_str()));
  }

  if (const SCEVUnknown *SU = dyn_cast<SCEVUnknown>(S))
    return valueToZ3Expr(SU->getValue());

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

  std::string Name =
      "scev_" + std::to_string(reinterpret_cast<uintptr_t>(S));
  return Z3Expr(Z3Expr::getContext().int_const(Name.c_str()));
}
