#include "Dataflow/Mono/Analyses/Inter/InterConstantPropagation.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <memory>

using namespace llvm;

namespace mono {
namespace {

ConstantPropagationValue makeTop() {
  return ConstantPropagationValue{ConstantPropagationTag::Top, 0};
}

ConstantPropagationValue makeBottom() {
  return ConstantPropagationValue{ConstantPropagationTag::Bottom, 0};
}

ConstantPropagationValue makeConst(int64_t Value) {
  return ConstantPropagationValue{ConstantPropagationTag::Const, Value};
}

bool isBottom(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Bottom;
}

bool isConst(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Const;
}

bool equalValue(const ConstantPropagationValue &Lhs,
                const ConstantPropagationValue &Rhs) {
  return Lhs.Tag == Rhs.Tag && Lhs.ConstValue == Rhs.ConstValue;
}

ConstantPropagationValue resolveValue(const ConstantPropagationMap &In,
                                      const Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return makeConst(CI->getSExtValue());
  }

  auto It = In.find(V);
  if (It != In.end()) {
    return It->second;
  }

  return makeTop();
}

ConstantPropagationValue evalBinaryOp(unsigned Opcode,
                                      const ConstantPropagationValue &Lhs,
                                      const ConstantPropagationValue &Rhs) {
  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeBottom();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case Instruction::Add:
    return makeConst(LV + RV);
  case Instruction::Sub:
    return makeConst(LV - RV);
  case Instruction::Mul:
    return makeConst(LV * RV);
  case Instruction::SDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV / RV);
  case Instruction::UDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) /
                     static_cast<uint64_t>(RV));
  case Instruction::SRem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV % RV);
  case Instruction::URem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) %
                     static_cast<uint64_t>(RV));
  default:
    return makeBottom();
  }
}

class InterMonoConstantPropagation
    : public InterMonoProblem<ConstantPropagationDomain> {
public:
  explicit InterMonoConstantPropagation(Function *Entry,
                                        lotus::AliasAnalysisWrapper *AA)
      : InterMonoProblem<ConstantPropagationDomain>(
            std::vector<Function *>{Entry}, AA),
        AA(AA) {}

  ConstantPropagationMap normalFlow(Instruction *Inst,
                                    const ConstantPropagationMap &In) override {
    ConstantPropagationMap Out = In;

    if (const auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeTop();
      }
      return Out;
    }

    if (const auto *Store = dyn_cast<StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      if (!Store->getValueOperand()->getType()->isIntegerTy()) {
        return Out;
      }

      auto Val = resolveValue(In, Store->getValueOperand());
      if (!isBottom(Val)) {
        Out[Ptr] = Val;
        writeAliases(Ptr, Val, Out);
      }
      return Out;
    }

    if (const auto *Load = dyn_cast<LoadInst>(Inst)) {
      auto It = In.find(Load->getPointerOperand());
      if (It != In.end()) {
        Out[Load] = It->second;
      } else {
        Out[Load] = resolveAliasValue(Load->getPointerOperand(), In);
      }
      return Out;
    }

    if (const auto *Op = dyn_cast<BinaryOperator>(Inst)) {
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op->getOpcode(), Lhs, Rhs);
      return Out;
    }

    return Out;
  }

  ConstantPropagationMap merge(const ConstantPropagationMap &Lhs,
                               const ConstantPropagationMap &Rhs) override {
    ConstantPropagationMap Out;
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It != Rhs.end() && equalValue(Entry.second, It->second)) {
        Out.insert(std::make_pair(Entry.first, Entry.second));
      }
    }
    return Out;
  }

  bool equal_to(const ConstantPropagationMap &Lhs,
                const ConstantPropagationMap &Rhs) override {
    return Lhs == Rhs;
  }

  ConstantPropagationMap callFlow(Instruction *CallSite, Function *Callee,
                                  const ConstantPropagationMap &In) override {
    ConstantPropagationMap Out;
    if (CallSite == nullptr || Callee == nullptr) {
      return Out;
    }

    auto *Call = dyn_cast<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    // Map constants from actual args to formal args.
    auto *FormalIt = Callee->arg_begin();
    for (auto &Actual : Call->args()) {
      if (FormalIt == Callee->arg_end()) {
        break;
      }
      if (FormalIt->getType()->isIntegerTy()) {
        Out[&*FormalIt] = resolveValue(In, Actual.get());
      }
      ++FormalIt;
    }

    // Preserve globals (very conservative).
    for (const auto &Entry : In) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
      }
    }

    return Out;
  }

  ConstantPropagationMap returnFlow(Instruction *CallSite, Function *Callee,
                                    Instruction *ExitStmt,
                                    Instruction *RetSite,
                                    const ConstantPropagationMap &In) override {
    (void)Callee;
    (void)RetSite;

    ConstantPropagationMap Out;
    for (const auto &Entry : In) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
      }
    }

    auto *Ret = dyn_cast_or_null<ReturnInst>(ExitStmt);
    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Ret == nullptr || Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    auto *RetVal = Ret->getReturnValue();
    if (RetVal == nullptr || !RetVal->getType()->isIntegerTy()) {
      return Out;
    }

    Out[CallSite] = resolveValue(In, RetVal);
    return Out;
  }

  ConstantPropagationMap callToRetFlow(Instruction *CallSite,
                                       Instruction *RetSite,
                                       ArrayRef<Function *> Callees,
                                       const ConstantPropagationMap &In) override {
    (void)RetSite;
    ConstantPropagationMap Out = In;

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    // Unknown/indirect call: we lose constant information about the return value.
    if (Callees.empty()) {
      Out[CallSite] = makeBottom();
      return Out;
    }

    // Multiple potential callees: conservatively mark as unknown here. The
    // precise constant, if any, will be re-established by returnFlow + merge.
    if (Callees.size() > 1) {
      Out[CallSite] = makeBottom();
    }
    return Out;
  }

  std::unordered_map<Instruction *, ConstantPropagationMap>
  initialSeeds() override {
    std::unordered_map<Instruction *, ConstantPropagationMap> Seeds;
    Function *F = this->getEntryPoints().empty() ? nullptr
                                                 : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = ConstantPropagationMap{};
    return Seeds;
  }

private:
  lotus::AliasAnalysisWrapper *AA;

  static ConstantPropagationValue weakJoin(const ConstantPropagationValue &OldVal,
                                           const ConstantPropagationValue &NewVal) {
    if (equalValue(OldVal, NewVal)) {
      return OldVal;
    }
    return makeBottom();
  }

  void collectAliasedPointers(
      const Value *Ptr, const ConstantPropagationMap &State,
      std::vector<const Value *> &MustAliases,
      std::vector<const Value *> &MayAliases) const {
    if (Ptr == nullptr || !Ptr->getType()->isPointerTy()) {
      return;
    }

    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *PtrBase = Ptr->stripPointerCasts();
    auto Add = [&](const Value *Candidate) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        return;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (PtrBase != nullptr && CandidateBase != nullptr &&
          PtrBase == CandidateBase) {
        MustAliases.push_back(Candidate);
        return;
      }
      if (Candidate == Ptr) {
        MustAliases.push_back(Candidate);
        return;
      }
      if (!HaveAA) {
        MayAliases.push_back(Candidate);
        return;
      }
      auto Res = AA->query(Ptr, Candidate);
      if (Res == AliasResult::MustAlias) {
        MustAliases.push_back(Candidate);
      } else if (Res != AliasResult::NoAlias) {
        MayAliases.push_back(Candidate);
      }
    };

    Add(Ptr);
    for (const auto &Entry : State) {
      Add(Entry.first);
    }
  }

  ConstantPropagationValue resolveAliasValue(const Value *Ptr,
                                             const ConstantPropagationMap &In) const {
    if (AA == nullptr || !AA->isInitialized() || Ptr == nullptr ||
        !Ptr->getType()->isPointerTy()) {
      return makeTop();
    }

    bool Found = false;
    ConstantPropagationValue Val = makeTop();
    for (const auto &Entry : In) {
      auto *Alias = Entry.first;
      if (Alias == nullptr || !Alias->getType()->isPointerTy()) {
        continue;
      }
      if (AA->query(Ptr, Alias) == AliasResult::NoAlias) {
        continue;
      }
      auto It = In.find(Alias);
      if (It == In.end()) {
        continue;
      }
      if (!Found) {
        Val = It->second;
        Found = true;
      } else if (!equalValue(Val, It->second)) {
        return makeBottom();
      }
    }
    return Found ? Val : makeTop();
  }

  void writeAliases(const Value *Ptr, const ConstantPropagationValue &Val,
                    ConstantPropagationMap &Out) const {
    std::vector<const Value *> MustAliases;
    std::vector<const Value *> MayAliases;
    collectAliasedPointers(Ptr, Out, MustAliases, MayAliases);

    for (const auto *Alias : MustAliases) {
      if (Alias != nullptr) {
        Out[Alias] = Val;
      }
    }

    for (const auto *Alias : MayAliases) {
      if (Alias == nullptr) {
        continue;
      }
      auto It = Out.find(Alias);
      auto OldVal = It != Out.end() ? It->second : makeTop();
      Out[Alias] = weakJoin(OldVal, Val);
    }
  }
};

} // namespace

InterMonoConstantPropagationAnalysisResult
runInterMonoConstantPropagation(Function *Entry) {
  InterMonoConstantPropagationAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *Entry->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  InterMonoConstantPropagation Problem(Entry, AA.get());
  InterMonoSolver<ConstantPropagationDomain, kDefaultConstantPropagationCallStringLength>
      Solver(Problem);
  Solver.solve();

  if (const auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoConstantPropagationResult>(*Raw);
  }
  return Result;
}

} // namespace mono
