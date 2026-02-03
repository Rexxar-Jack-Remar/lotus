#include "Dataflow/Mono/Analyses/Interprocedural/InterMonoTaintAnalysis.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/InterMonoSolver.h"

#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace mono {
namespace {

struct TaintDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class InterMonoTaintProblem : public InterMonoProblem<TaintDomain> {
public:
  using mono_container_t = typename TaintDomain::mono_container_t;

  InterMonoTaintProblem(Function *Entry, const InterMonoTaintConfig &Config)
      : InterMonoProblem<TaintDomain>({Entry}), Config(Config) {}

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
      if (In.count(Store->getValueOperand())) {
        Out.insert(Store->getPointerOperand());
      }
      return Out;
    }

    if (!Inst->getType()->isVoidTy()) {
      for (auto &Op : Inst->operands()) {
        if (In.count(Op.get())) {
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
            Out.insert(Arg.get());
          }
        }
      }
    }

    return Out;
  }

  const InterMonoTaintConfig &Config;
  InterMonoTaintReport Report;
};

} // namespace

InterMonoTaintAnalysisResult
runInterMonoTaintAnalysis(Function *Entry, const InterMonoTaintConfig &Config) {
  InterMonoTaintAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  InterMonoTaintProblem Problem(Entry, Config);
  InterMonoSolver<TaintDomain, kDefaultTaintCallStringLength> Solver(Problem);
  Solver.solve();

  if (auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoTaintResult>(*Raw);
  }
  Result.Report = Problem.getReport();
  return Result;
}

} // namespace mono
