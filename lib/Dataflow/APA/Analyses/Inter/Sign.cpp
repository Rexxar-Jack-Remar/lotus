#include "Dataflow/APA/Analyses/Inter/Sign.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"
#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"

#include <memory>
#include <unordered_map>

namespace elimination {
namespace {

struct InterSignAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = SignMap;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
  using abstract_domain_t = SignDomain;
};

bool isIntegerLike(const llvm::Value *V) {
  return V != nullptr && V->getType()->isIntegerTy();
}

SignValue signOfConstant(const llvm::Constant *C) {
  if (const auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(C)) {
    if (CI->isZero())
      return SignValue::zero();
    return CI->isNegative() ? SignValue::negative() : SignValue::positive();
  }
  return SignValue::top();
}

const llvm::Value *getMemKey(const llvm::Value *Ptr) {
  auto *Base = llvm::getUnderlyingObject(Ptr);
  return Base != nullptr ? Base : Ptr;
}

SignValue resolveValue(const SignMap &In, const llvm::Value *V) {
  if (auto *GV = llvm::dyn_cast_or_null<llvm::GlobalVariable>(V)) {
    if (GV->isConstant() && GV->hasInitializer() &&
        GV->getInitializer()->getType()->isIntegerTy()) {
      return signOfConstant(GV->getInitializer());
    }
  }
  if (auto *C = llvm::dyn_cast_or_null<llvm::Constant>(V))
    return signOfConstant(C);
  auto It = In.find(V);
  return It != In.end() ? It->second : SignValue::bottom();
}

SignValue negate(SignValue V) {
  std::uint8_t Mask = SignValue::None;
  if (V.mayBeNegative())
    Mask |= SignValue::Positive;
  if (V.mayBeZero())
    Mask |= SignValue::Zero;
  if (V.mayBePositive())
    Mask |= SignValue::Negative;
  return SignValue(Mask);
}

SignValue addSigns(SignValue L, SignValue R) {
  std::uint8_t Mask = SignValue::None;
  auto AddCase = [&](SignValue A, SignValue B, SignValue Result) {
    if ((L.bits() & A.bits()) != 0 && (R.bits() & B.bits()) != 0)
      Mask |= Result.bits();
  };
  AddCase(SignValue::negative(), SignValue::negative(), SignValue::negative());
  AddCase(SignValue::negative(), SignValue::zero(), SignValue::negative());
  AddCase(SignValue::zero(), SignValue::negative(), SignValue::negative());
  AddCase(SignValue::zero(), SignValue::zero(), SignValue::zero());
  AddCase(SignValue::zero(), SignValue::positive(), SignValue::positive());
  AddCase(SignValue::positive(), SignValue::zero(), SignValue::positive());
  AddCase(SignValue::positive(), SignValue::positive(), SignValue::positive());
  AddCase(SignValue::negative(), SignValue::positive(), SignValue::top());
  AddCase(SignValue::positive(), SignValue::negative(), SignValue::top());
  return SignValue(Mask);
}

SignValue mulSigns(SignValue L, SignValue R) {
  std::uint8_t Mask = SignValue::None;
  if (L.mayBeZero() || R.mayBeZero())
    Mask |= SignValue::Zero;
  if ((L.mayBeNegative() && R.mayBeNegative()) ||
      (L.mayBePositive() && R.mayBePositive())) {
    Mask |= SignValue::Positive;
  }
  if ((L.mayBeNegative() && R.mayBePositive()) ||
      (L.mayBePositive() && R.mayBeNegative())) {
    Mask |= SignValue::Negative;
  }
  return SignValue(Mask);
}

SignValue evalBinaryOp(const llvm::BinaryOperator *Op, SignValue L,
                       SignValue R) {
  if (Op == nullptr || L.isBottom() || R.isBottom())
    return SignValue::bottom();
  switch (Op->getOpcode()) {
  case llvm::Instruction::Add:
    return addSigns(L, R);
  case llvm::Instruction::Sub:
    return addSigns(L, negate(R));
  case llvm::Instruction::Mul:
    return mulSigns(L, R);
  case llvm::Instruction::SDiv:
  case llvm::Instruction::SRem:
    return R.mayBeZero() ? SignValue::top() : mulSigns(L, R);
  case llvm::Instruction::UDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::LShr:
    return SignValue::nonNegative();
  case llvm::Instruction::And:
    return L == SignValue::zero() || R == SignValue::zero() ? SignValue::zero()
                                                            : SignValue::top();
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    if (L == SignValue::zero())
      return R;
    if (R == SignValue::zero())
      return L;
    return SignValue::top();
  default:
    return SignValue::top();
  }
}

SignValue evalCast(const llvm::CastInst *Cast, SignValue Src) {
  if (Cast == nullptr || Src.isBottom() || !Cast->getType()->isIntegerTy())
    return SignValue::bottom();
  switch (Cast->getOpcode()) {
  case llvm::Instruction::ZExt:
  case llvm::Instruction::PtrToInt:
    return SignValue::nonNegative();
  case llvm::Instruction::SExt:
    return Src;
  case llvm::Instruction::Trunc:
    return Cast->getType()->isIntegerTy(1) ? SignValue::nonNegative()
                                           : SignValue::top();
  default:
    return SignValue::top();
  }
}

void clobberMemoryByCall(const llvm::CallBase *Call, SignMap &Out) {
  if (Call == nullptr || !Call->mayWriteToMemory())
    return;
  std::vector<const llvm::Value *> Keys;
  for (const auto &Entry : Out) {
    if (Entry.first != nullptr && Entry.first->getType()->isPointerTy())
      Keys.push_back(Entry.first);
  }
  for (auto *Key : Keys)
    Out.set(Key, SignValue::top());
}

class InterSignProblem final
    : public LLVMInterEliminationProblem<InterSignAnalysisTypes> {
public:
  InterSignProblem(llvm::Function *Entry,
                   const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterSignAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF) {
    auto *M = Entry != nullptr ? Entry->getParent() : nullptr;
    if (M == nullptr)
      return;
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
            MemoryKeys[&I] = getMemKey(Store->getPointerOperand());
          else if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            MemoryKeys[&I] = getMemKey(Load->getPointerOperand());
        }
      }
    }
  }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    if (Inst == nullptr)
      return Out;

    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy())
        Out[Alloca] = SignValue::bottom();
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      if (Store->getValueOperand()->getType()->isIntegerTy()) {
        Out[cachedMemKey(Store, Store->getPointerOperand())] =
            resolveValue(In, Store->getValueOperand());
      }
    } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (Load->getType()->isIntegerTy()) {
        auto It = In.find(cachedMemKey(Load, Load->getPointerOperand()));
        Out[Load] = It != In.end() ? It->second : SignValue::top();
      }
    } else if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(Inst)) {
      if (Op->getType()->isIntegerTy()) {
        Out[Op] = evalBinaryOp(Op, resolveValue(In, Op->getOperand(0)),
                               resolveValue(In, Op->getOperand(1)));
      }
    } else if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      if (Cast->getType()->isIntegerTy())
        Out[Cast] = evalCast(Cast, resolveValue(In, Cast->getOperand(0)));
    } else if (const auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(Inst)) {
      Out[ICmp] = SignValue::nonNegative();
    } else if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (Select->getType()->isIntegerTy()) {
        auto Cond = resolveValue(In, Select->getCondition());
        auto Value = Cond == SignValue::zero()
                         ? resolveValue(In, Select->getFalseValue())
                         : resolveValue(In, Select->getTrueValue());
        if (Cond.mayBeZero() && Cond != SignValue::zero())
          Value.mergeIn(resolveValue(In, Select->getFalseValue()));
        Out[Select] = Value;
      }
    } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      SignValue Value = SignValue::bottom();
      for (const auto &Incoming : Phi->incoming_values())
        Value.mergeIn(resolveValue(In, Incoming.get()));
      Out[Phi] = Value;
    } else if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Inst)) {
      if (Freeze->getType()->isIntegerTy())
        Out[Freeze] = resolveValue(In, Freeze->getOperand(0));
    } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      clobberMemoryByCall(Call, Out);
    } else if (isIntegerLike(Inst)) {
      Out[Inst] = SignValue::top();
    }
    return Out;
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out = this->bottom();
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr)
      return Out;
    llvm_inter::forEachActualFormalPair(
        Call, Callee,
        [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned) {
          Out[Formal] = resolveValue(In, Actual);
          if (Formal->getType()->isPointerTy()) {
            auto It = In.find(getMemKey(Actual));
            if (It != In.end())
              Out[Formal] = It->second;
          }
        });
    llvm_inter::copyGlobalValueFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t,
                    const fact_t &In) override {
    fact_t Out = this->bottom();
    llvm_inter::copyGlobalValueFacts(In, Out);
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr)
      return Out;
    llvm_inter::forEachActualFormalPair(
        Call, Callee,
        [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned) {
          if (!Formal->getType()->isPointerTy())
            return;
          auto It = In.find(Formal);
          if (It != In.end())
            Out[getMemKey(Actual)] = It->second;
        });
    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    if (llvm_inter::hasConcreteReturnValue(Call, Ret))
      Out[CallSite] = resolveValue(In, Ret->getReturnValue());
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t, const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    fact_t Out = In;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Call->getType()->isVoidTy())
      return Out;
    bool AllDefined = !Callees.empty();
    for (auto *Callee : Callees)
      AllDefined &=
          Callee != nullptr && !Callee->isDeclaration() && !Callee->empty();
    if (AllDefined) {
      Out.erase(CallSite);
    } else {
      Out[CallSite] = SignValue::top();
    }
    return Out;
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry != nullptr && !Entry->empty())
      Seeds[&Entry->getEntryBlock().front()] = this->bottom();
    return Seeds;
  }

private:
  std::unordered_map<const llvm::Instruction *, const llvm::Value *> MemoryKeys;

  const llvm::Value *cachedMemKey(const llvm::Instruction *Inst,
                                  const llvm::Value *Ptr) const {
    auto It = MemoryKeys.find(Inst);
    return It != MemoryKeys.end() ? It->second : getMemKey(Ptr);
  }
};

} // namespace

InterSignResult runInterElimSign(llvm::Function *Entry,
                                 const dataflow::controlflow::InterCFG *ICF) {
  InterSignResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterSignProblem Problem(Entry, ICF);
  InterEliminationSolver<InterSignAnalysisTypes,
                         kDefaultInterElimSignCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  return Out;
}

InterSignResult
runInterSummaryElimSign(llvm::Function *Entry,
                        const dataflow::controlflow::InterCFG *ICF,
                        PathSummaryEquationOptions Options) {
  InterSignResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterSignProblem Problem(Entry, ICF);
  ForwardInterSummarySolver<InterSignAnalysisTypes,
                            kDefaultInterElimSignCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  Out.setSummarySolveDiagnostics(Solver.resultDiagnostics());
  return Out;
}

} // namespace elimination
