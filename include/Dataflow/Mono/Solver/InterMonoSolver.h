#ifndef ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_
#define ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_

#include "Dataflow/Mono/MonoFramework.h"
#include "Dataflow/Mono/Solver/CallStringInterProceduralDataFlow.h"

#include "llvm/IR/Instructions.h"

#include <memory>
#include <set>
#include <vector>

namespace mono {

template <typename AnalysisDomainTy, unsigned K> class InterMonoSolver {
public:
  using ProblemTy = InterMonoProblem<AnalysisDomainTy>;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using ResultTy = dataflow::ContextSensitiveDataFlowResult<K, mono_container_t>;
  using Context = typename ResultTy::Context;

  explicit InterMonoSolver(ProblemTy &Problem) : Problem(Problem) {}

  void solve() {
    auto &Entries = Problem.getEntryPoints();
    if (Entries.empty()) {
      return;
    }

    dataflow::CallStringInterProceduralDataFlowEngine<K, mono_container_t>
        Engine;
    auto ComputeGEN = [this](llvm::Instruction *Inst, ResultTy *DF) {
      computeGEN(Inst, DF);
    };
    auto ComputeKILL = [this](llvm::Instruction *Inst, ResultTy *DF) {
      computeKILL(Inst, DF);
    };
    auto InitializeIN = [this](llvm::Instruction *Inst, mono_container_t &IN) {
      initializeIN(Inst, IN);
    };
    auto InitializeOUT = [this](llvm::Instruction *Inst,
                                mono_container_t &OUT) {
      initializeOUT(Inst, OUT);
    };
    auto ComputeIN = [this](llvm::Instruction *Inst, llvm::Instruction *PredInst,
                            const Context &PredCtx,
                            mono_container_t &IN, ResultTy *DF) {
      computeIN(Inst, PredInst, PredCtx, IN, DF);
    };
    auto ComputeOUT = [this](llvm::Instruction *Inst, const Context &Ctx,
                             mono_container_t &OUT, ResultTy *DF) {
      computeOUT(Inst, Ctx, OUT, DF);
    };

    auto *Raw = Engine.applyForward(
        Entries.front(), ComputeGEN, ComputeKILL, InitializeIN, InitializeOUT,
        ComputeIN, ComputeOUT);
    Result.reset(Raw);
  }

  [[nodiscard]] const ResultTy *getResults() const { return Result.get(); }

private:
  static bool isFunctionEntry(llvm::Instruction *Inst) {
    auto *BB = Inst->getParent();
    return &BB->getParent()->getEntryBlock() == BB &&
           Inst == &*BB->begin();
  }

  static llvm::Function *getDirectCallee(llvm::Instruction *Inst) {
    auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst);
    if (Call == nullptr) {
      return nullptr;
    }
    return Call->getCalledFunction();
  }

  static std::vector<llvm::Instruction *>
  continuationInstructions(llvm::Instruction *CallInst) {
    std::vector<llvm::Instruction *> Continuations;
    if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(CallInst)) {
      auto *NormalDest = Invoke->getNormalDest();
      Continuations.push_back(&*NormalDest->begin());
      return Continuations;
    }
    if (auto *Next = CallInst->getNextNode()) {
      Continuations.push_back(Next);
    }
    return Continuations;
  }

  static bool isContinuationOfCall(llvm::Instruction *Inst,
                                   llvm::Instruction *CallInst) {
    for (auto *Cont : continuationInstructions(CallInst)) {
      if (Cont == Inst) {
        return true;
      }
    }
    return false;
  }

  void initializeIN(llvm::Instruction *, mono_container_t &IN) {
    IN = Problem.allTop();
  }

  void initializeOUT(llvm::Instruction *, mono_container_t &OUT) {
    OUT = Problem.allTop();
  }

  void computeGEN(llvm::Instruction *Inst, ResultTy *DF) {
    auto &Gen = DF->GEN(Inst);
    Gen = Problem.allTop();
  }

  void computeKILL(llvm::Instruction *Inst, ResultTy *DF) {
    auto &Kill = DF->KILL(Inst);
    Kill = Problem.allTop();
  }

  void computeIN(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                 const Context &PredCtx, mono_container_t &IN, ResultTy *DF) {
    mono_container_t Incoming;
    auto &PredIn = DF->IN(PredInst, PredCtx);

    if (isFunctionEntry(Inst) && llvm::isa<llvm::CallBase>(PredInst) &&
        getDirectCallee(PredInst) == Inst->getFunction()) {
      Incoming = Problem.callFlow(PredInst, Inst->getFunction(), PredIn);
    } else if (llvm::isa<llvm::ReturnInst>(PredInst)) {
      Context CallerCtx = PredCtx;
      auto *CallSite = CallerCtx.pop_back();
      if (CallSite != nullptr) {
        Incoming = Problem.returnFlow(CallSite, PredInst->getFunction(),
                                      PredInst, Inst, PredIn);
      }
    } else if (llvm::isa<llvm::CallBase>(PredInst) &&
               isContinuationOfCall(Inst, PredInst)) {
      std::vector<llvm::Function *> Callees;
      if (auto *Callee = getDirectCallee(PredInst)) {
        Callees.push_back(Callee);
      }
      Incoming = Problem.callToRetFlow(PredInst, Inst, Callees, PredIn);
    } else {
      Incoming = Problem.normalFlow(PredInst, PredIn);
    }

    if (IN.empty()) {
      IN = Incoming;
    } else {
      IN = Problem.merge(IN, Incoming);
    }
  }

  void computeOUT(llvm::Instruction *Inst, const Context &Ctx,
                  mono_container_t &OUT, ResultTy *DF) {
    OUT = Problem.normalFlow(Inst, DF->IN(Inst, Ctx));
  }

  ProblemTy &Problem;
  std::unique_ptr<ResultTy> Result;
};

} // namespace mono

#endif // ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_
