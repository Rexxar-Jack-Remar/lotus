#include "Verification/LoopInvariants/FunctionInvariantProver.h"

#include "Verification/LoopInvariants/Z3ValueNaming.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "z3++.h"

using namespace llvm;
using namespace lotus;

std::string FunctionInvariantProver::getValueName(const Value *V) {
  return z3NameForValue(V);
}

FunctionInvariantProver::FunctionInvariantProver(const Function &F,
                                                 ScalarEvolution &SE)
    : Func(F), SE(SE), Ctx(&Z3Expr::getContext()) {}

FunctionInvariantProver::~FunctionInvariantProver() = default;

FunctionInvariantProver::ProofResult FunctionInvariantProver::proveInvariant(
    const FunctionInvariantCandidate &Candidate) {
  if (Candidate.Formula.id() == 0)
    return ProofResult(false, "Invalid formula");

  z3::solver Solver(*Ctx);

  ProofResult ExitResult = proveAtExit(Candidate.Formula.getExpr(), Solver);
  return ExitResult;
}

static z3::expr translateValue(z3::context &Ctx, const Function &F,
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

  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(V)) {
    const Value *Op0 = BO->getOperand(0);
    const Value *Op1 = BO->getOperand(1);
    z3::expr E0 = translateValue(Ctx, F, Op0);
    z3::expr E1 = translateValue(Ctx, F, Op1);
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

  if (const CastInst *Cast = dyn_cast<CastInst>(V)) {
    return translateValue(Ctx, F, Cast->getOperand(0));
  }

  // Fall back to an uninterpreted integer.
  std::string Name = lotus::z3NameForValue(V);
  return Ctx.int_const(Name.c_str());
}

FunctionInvariantProver::ProofResult
FunctionInvariantProver::proveAtExit(const z3::expr &Invariant,
                                     z3::solver &Solver) {
  z3::expr RetVar = Ctx->int_const("ret");

  bool SawReturn = false;
  for (const BasicBlock &BB : Func) {
    for (const Instruction &I : BB) {
      const ReturnInst *RI = dyn_cast<ReturnInst>(&I);
      if (!RI)
        continue;
      const Value *RetVal = RI->getReturnValue();
      if (!RetVal)
        continue;

      SawReturn = true;
      Solver.push();

      Solver.add(RetVar == translateValue(*Ctx, Func, RetVal));

      z3::expr NotInv = !Invariant;
      Solver.add(NotInv);

      z3::check_result Result = Solver.check();
      if (Result == z3::sat) {
        z3::model Model = Solver.get_model();
        llvm::errs() << "DEBUG: Function exit counterexample model: " << Model
                     << "\n";
        Solver.pop();
        return ProofResult(
            false, "Exit case fails: invariant does not hold at function exit");
      }
      if (Result == z3::unknown) {
        Solver.pop();
        return ProofResult(false, "Exit case unknown");
      }

      Solver.pop();
    }
  }

  if (!SawReturn)
    return ProofResult(false, "No return values to reason about");

  return ProofResult(true);
}
