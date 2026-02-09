#include "Dataflow/Mono/Analyses/Interprocedural/InterMonoTaintAnalysis.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/InterMonoSolver.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include "llvm/IR/Instructions.h"

#include <memory>

using namespace llvm;

namespace mono {
namespace {

struct TaintDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class InterMonoTaintProblem : public InterMonoProblem<TaintDomain> {
public:
  using mono_container_t = typename TaintDomain::mono_container_t;

  InterMonoTaintProblem(Function *Entry, const InterMonoTaintConfig &Config,
                        lotus::AliasAnalysisWrapper *AA)
      : InterMonoProblem<TaintDomain>({Entry}, AA), Config(Config), AA(AA) {}

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    if (auto *Call = dyn_cast<CallBase>(Inst)) {
      return applyCallSite(Call, In);
    }
    return applyInstructionFlow(Inst, In);
  }

  mono_container_t merge(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  mono_container_t callFlow(Instruction *CallSite, Function *Callee,
                            const mono_container_t &In) override {
    mono_container_t Out;
    if (Callee == nullptr) {
      return Out;
    }

    auto *Call = dyn_cast<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    auto *ArgIt = Callee->arg_begin();
    for (auto &Arg : Call->args()) {
      if (ArgIt == Callee->arg_end()) {
        break;
      }
      if (In.count(Arg.get())) {
        Out.insert(&*ArgIt);
      }
      ++ArgIt;
    }

    for (auto *Val : In) {
      if (isa<GlobalValue>(Val)) {
        Out.insert(Val);
      }
    }

    return Out;
  }

  mono_container_t returnFlow(Instruction *CallSite, Function *Callee,
                              Instruction *ExitStmt, Instruction *RetSite,
                              const mono_container_t &In) override {
    (void)Callee;
    (void)RetSite;

    mono_container_t Out;
    for (auto *Val : In) {
      if (isa<GlobalValue>(Val)) {
        Out.insert(Val);
      }
    }

    auto *Ret = dyn_cast<ReturnInst>(ExitStmt);
    if (Ret != nullptr && CallSite != nullptr) {
      if (auto *RetVal = Ret->getReturnValue()) {
        if (In.count(RetVal) && !CallSite->getType()->isVoidTy()) {
          Out.insert(CallSite);
        }
      }
    }

    return Out;
  }

  mono_container_t callToRetFlow(Instruction *CallSite, Instruction *RetSite,
                                 ArrayRef<Function *> Callees,
                                 const mono_container_t &In) override {
    (void)RetSite;
    (void)Callees;
    if (auto *Call = dyn_cast<CallBase>(CallSite)) {
      return applyCallSite(Call, In);
    }
    return In;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = {};
    return Seeds;
  }

  const InterMonoTaintReport &getReport() const {
    return Report;
  }

private:
  bool isTaintedValue(const Value *V, const mono_container_t &In) const {
    if (V == nullptr) {
      return false;
    }
    if (In.count(const_cast<Value *>(V))) {
      return true;
    }
    if (AA == nullptr || !AA->isInitialized()) {
      return false;
    }
    if (!V->getType()->isPointerTy()) {
      return false;
    }
    std::vector<const Value *> Aliases;
    if (!AA->getAliasSet(V, Aliases)) {
      return false;
    }
    for (const auto *Alias : Aliases) {
      if (In.count(const_cast<Value *>(Alias))) {
        return true;
      }
    }
    return false;
  }

  void taintAliases(Value *Ptr, mono_container_t &Out) const {
    if (Ptr == nullptr) {
      return;
    }
    Out.insert(Ptr);
    if (AA == nullptr || !AA->isInitialized()) {
      return;
    }
    if (!Ptr->getType()->isPointerTy()) {
      return;
    }
    std::vector<const Value *> Aliases;
    if (!AA->getAliasSet(Ptr, Aliases)) {
      return;
    }
    for (const auto *Alias : Aliases) {
      Out.insert(const_cast<Value *>(Alias));
    }
  }

  bool isSourceFunction(const Function *F) const {
    if (F == nullptr) {
      return false;
    }
    return Config.SourceFunctions.count(F->getName().str()) > 0;
  }

  bool isSinkFunction(const Function *F) const {
    if (F == nullptr) {
      return false;
    }
    return Config.SinkFunctions.count(F->getName().str()) > 0;
  }

  void recordSinkLeak(CallBase *Call, const mono_container_t &In) {
    auto *Callee = Call->getCalledFunction();
    if (!isSinkFunction(Callee)) {
      return;
    }
    for (auto &Arg : Call->args()) {
      if (In.count(Arg.get())) {
        Report.Leaks[Call].insert(Arg.get());
      }
    }
  }

  mono_container_t applyInstructionFlow(Instruction *Inst,
                                        const mono_container_t &In) {
    mono_container_t Out = In;

    if (auto *Store = dyn_cast<StoreInst>(Inst)) {
      if (isTaintedValue(Store->getValueOperand(), In)) {
        taintAliases(Store->getPointerOperand(), Out);
      }
      return Out;
    }

    if (auto *Load = dyn_cast<LoadInst>(Inst)) {
      if (isTaintedValue(Load->getPointerOperand(), In)) {
        Out.insert(Load);
      }
      return Out;
    }

    if (!Inst->getType()->isVoidTy()) {
      for (auto &Op : Inst->operands()) {
        if (isTaintedValue(Op.get(), In)) {
          Out.insert(Inst);
          break;
        }
      }
    }

    return Out;
  }

  mono_container_t applyCallSite(CallBase *Call, const mono_container_t &In) {
    mono_container_t Out = applyInstructionFlow(Call, In);
    auto *Callee = Call->getCalledFunction();

    recordSinkLeak(Call, In);

    if (isSourceFunction(Callee)) {
      if (!Call->getType()->isVoidTy()) {
        Out.insert(Call);
      }
      if (Config.TaintPointerArgsFromSources) {
        for (auto &Arg : Call->args()) {
          if (Arg->getType()->isPointerTy()) {
            taintAliases(Arg.get(), Out);
          }
        }
      }
    }

    return Out;
  }

  const InterMonoTaintConfig &Config;
  InterMonoTaintReport Report;
  lotus::AliasAnalysisWrapper *AA;
};

} // namespace

InterMonoTaintAnalysisResult
runInterMonoTaintAnalysis(Function *Entry, const InterMonoTaintConfig &Config) {
  InterMonoTaintAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *Entry->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::BasicAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  InterMonoTaintProblem Problem(Entry, Config, AA.get());
  InterMonoSolver<TaintDomain, kDefaultTaintCallStringLength> Solver(Problem);
  Solver.solve();

  if (auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoTaintResult>(*Raw);
  }
  Result.Report = Problem.getReport();
  return Result;
}

} // namespace mono
