#ifndef ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_
#define ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_

#include "Dataflow/Mono/InterMonoProblem.h"
#include "Dataflow/Mono/ControlFlow/InterCFG.h"
#include "Dataflow/Mono/Solver/CallStringInterProceduralDataFlow.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
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
  using ContextKey = typename ResultTy::ContextKey;
  using ICFG = mono::InterCFG;

  explicit InterMonoSolver(ProblemTy &Problem) : Problem(Problem) {}

  void solve() {
    auto &Entries = Problem.getEntryPoints();
    const auto Seeds = Problem.initialSeeds();
    if (Entries.empty() && Seeds.empty()) {
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
    auto Equal = [this](const mono_container_t &Lhs, const mono_container_t &Rhs) {
      return Problem.equal_to(Lhs, Rhs);
    };
    auto GetCallees = [this](llvm::Instruction *Inst) {
      return getCalleesOfCallAt(Inst);
    };

    std::vector<ContextKey> RootKeys;
    std::map<ContextKey, mono_container_t> SeedIns;

    Context EmptyCtx;
    for (const auto &Seed : Seeds) {
      RootKeys.push_back(ContextKey{Seed.first, EmptyCtx});
      SeedIns[ContextKey{Seed.first, EmptyCtx}] = Seed.second;
    }

    if (RootKeys.empty()) {
      // Default seed: start at each entry's first instruction with TOP.
      for (auto *Entry : Entries) {
        if (Entry == nullptr || Entry->isDeclaration() || Entry->empty()) {
          continue;
        }
        RootKeys.push_back(ContextKey{&*Entry->getEntryBlock().begin(), EmptyCtx});
      }
    }

    llvm::Module *M = nullptr;
    if (!RootKeys.empty() && RootKeys.front().Inst != nullptr) {
      M = RootKeys.front().Inst->getModule();
    }

    // Select ICFG (provided by the problem or LLVM fallback).
    OwnedICF.reset();
    ICF = nullptr;
    if (auto *Provided = Problem.getICFG()) {
      ICF = Provided;
    } else {
      auto GetCallees = [this](llvm::Instruction *Inst) {
        return Problem.getCalleesOfCallAt(Inst);
      };
      OwnedICF = std::make_unique<LLVMInterCFG>(M, GetCallees);
      ICF = OwnedICF.get();
    }

    auto *Raw = Engine.applyForwardFromSeeds(
        M, RootKeys, ICF, SeedIns, ComputeGEN, ComputeKILL, InitializeIN,
        InitializeOUT, ComputeIN, ComputeOUT, Equal, GetCallees);
    Result.reset(Raw);
  }

  const ResultTy *getResults() const { return Result.get(); }

  /// Returns the IN facts at \p Stmt merged across all call-string contexts.
  /// Uses the problem's merge to combine per-context facts. Returns an empty
  /// container if no results or \p Stmt has no entries.
  mono_container_t getResultsAt(llvm::Instruction *Stmt) const {
    if (!Result) {
      return mono_container_t{};
    }
    mono_container_t merged;
    bool first = true;
    for (const auto &Cell : Result->getINMap()) {
      if (Cell.first.Inst != Stmt) {
        continue;
      }
      if (first) {
        merged = Cell.second;
        first = false;
      } else {
        merged = Problem.merge(merged, Cell.second);
      }
    }
    return merged;
  }

  /// Raw IN map: (Instruction, Context) -> facts. Null if solve() not run or
  /// produced no results.
  const std::map<ContextKey, mono_container_t> *getAnalysisINMap() const {
    return Result ? &Result->getINMap() : nullptr;
  }

  /// Raw OUT map: (Instruction, Context) -> facts. Null if solve() not run or
  /// produced no results.
  const std::map<ContextKey, mono_container_t> *getAnalysisOUTMap() const {
    return Result ? &Result->getOUTMap() : nullptr;
  }

  void dumpResults(llvm::raw_ostream &OS = llvm::outs()) const {
    OS << "\n================ InterMonoSolver results ================\n";
    if (!Result) {
      OS << "No results computed!\n";
      return;
    }
    for (const auto &Cell : Result->getINMap()) {
      const auto &Key = Cell.first;
      const auto &Facts = Cell.second;
      OS << "Instruction: ";
      if (Key.Inst != nullptr) {
        OS << *Key.Inst;
      } else {
        OS << "<null>";
      }
      OS << "\n";
      Key.Ctx.print(OS) << "\n";
      OS << "Facts: ";
      if (Facts.empty()) {
        OS << "EMPTY\n";
      } else {
        Problem.printContainer(OS, Facts);
        OS << "\n";
      }
    }
  }

  void emitTextReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}
  void emitGraphicalReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}

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
    if (auto *CallBr = llvm::dyn_cast<llvm::CallBrInst>(CallInst)) {
      for (unsigned I = 0, E = CallBr->getNumSuccessors(); I < E; ++I) {
        Continuations.push_back(&*CallBr->getSuccessor(I)->begin());
      }
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

  std::vector<llvm::Function *> getCalleesOfCallAt(llvm::Instruction *Inst) const {
    return Problem.getCalleesOfCallAt(Inst);
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

    if (isFunctionEntry(Inst) && llvm::isa<llvm::CallBase>(PredInst)) {
      const auto Callees = getCalleesOfCallAt(PredInst);
      bool Matches = false;
      for (auto *Callee : Callees) {
        if (Callee == Inst->getFunction()) {
          Matches = true;
          break;
        }
      }
      if (Matches) {
        Incoming = Problem.callFlow(PredInst, Inst->getFunction(), PredIn);
      } else {
        Incoming = Problem.allTop();
      }
    } else if (llvm::isa<llvm::ReturnInst>(PredInst)) {
      Context CallerCtx = PredCtx;
      auto *CallSite = CallerCtx.pop_back();
      if (CallSite != nullptr) {
        Incoming = Problem.returnFlow(CallSite, PredInst->getFunction(),
                                      PredInst, Inst, PredIn);
      } else {
        // Empty context: fan out to all callers of the current callee and
        // merge their return-flow facts. Explicitly start with allTop() so
        // behavior is correct when ICF is null or there are no callers.
        Incoming = Problem.allTop();
        if (ICF != nullptr) {
          mono_container_t Merged;
          for (auto *Caller : ICF->getCallersOf(PredInst->getFunction())) {
            auto RetFacts = Problem.returnFlow(
                Caller, PredInst->getFunction(), PredInst, Inst, PredIn);
            if (Merged.empty()) {
              Merged = RetFacts;
            } else {
              Merged = Problem.merge(Merged, RetFacts);
            }
          }
          if (!Merged.empty()) {
            Incoming = Merged;
          }
        }
      }
    } else if (llvm::isa<llvm::CallBase>(PredInst) &&
               isContinuationOfCall(Inst, PredInst)) {
      const auto Callees = getCalleesOfCallAt(PredInst);
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
  std::unique_ptr<LLVMInterCFG> OwnedICF;
  const ICFG *ICF = nullptr;
};

template <typename Problem, unsigned K>
using InterMonoSolver_P =
    InterMonoSolver<typename Problem::ProblemAnalysisDomain, K>;

} // namespace mono

#endif // ANALYSIS_MONO_SOLVER_INTERMONOSOLVER_H_
