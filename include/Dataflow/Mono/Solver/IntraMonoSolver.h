#ifndef ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_
#define ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_

#include "Dataflow/Mono/IntraMonoProblem.h"
#include "Dataflow/Mono/ControlFlow/IntraCFG.h"

#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mono {

template <typename AnalysisDomainTy> class IntraMonoSolver {
public:
  using ProblemTy = IntraMonoProblem<AnalysisDomainTy>;
  using n_t = typename AnalysisDomainTy::n_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;
  using CFGTy = typename AnalysisDomainTy::c_t;

  explicit IntraMonoSolver(ProblemTy &Problem)
      : Problem(Problem), CFG(selectCFG()) {}

  void solve() {
    initialize();
    while (!Worklist.empty()) {
      auto Edge = Worklist.front();
      Worklist.pop_front();
      auto Src = Edge.first;
      auto Dst = Edge.second;

      auto Out = Problem.normalFlow(Src, AnalysisIn[Src]);
      if (CFG->isBranchTarget(Src, Dst, Problem.direction())) {
        for (auto Pred : CFG->getPredsOf(Dst, Problem.direction())) {
          if (Pred == Src) {
            continue;
          }
          auto OtherOut = Problem.normalFlow(Pred, AnalysisIn[Pred]);
          Out = Problem.merge(Out, OtherOut);
        }
      }

      if (!Problem.equal_to(Out, AnalysisIn[Dst])) {
        AnalysisIn[Dst] = Out;
        for (auto Succ : CFG->getSuccsOf(Dst, Problem.direction())) {
          Worklist.push_back({Dst, Succ});
        }
      }
    }

    for (auto &Entry : AnalysisIn) {
      AnalysisOut[Entry.first] = Problem.normalFlow(Entry.first, Entry.second);
    }
  }

  const mono_container_t &getResultsAt(n_t Stmt) const { return getInResultsAt(Stmt); }

  const mono_container_t &getInResultsAt(n_t Stmt) const {
    auto It = AnalysisIn.find(Stmt);
    if (It != AnalysisIn.end()) {
      return It->second;
    }
    return DefaultValue;
  }

  const mono_container_t &getOutResultsAt(n_t Stmt) const {
    auto It = AnalysisOut.find(Stmt);
    if (It != AnalysisOut.end()) {
      return It->second;
    }
    return DefaultValue;
  }

  const std::unordered_map<n_t, mono_container_t> &getInResults() const {
    return AnalysisIn;
  }

  const std::unordered_map<n_t, mono_container_t> &getOutResults() const {
    return AnalysisOut;
  }

  void dumpResults(llvm::raw_ostream &OS = llvm::outs()) const {
    OS << "\n================ IntraMonoSolver results ================\n";
    if (AnalysisIn.empty()) {
      OS << "No results computed!\n";
      return;
    }
    std::vector<std::pair<n_t, mono_container_t>> Cells;
    Cells.reserve(AnalysisIn.size());
    Cells.insert(Cells.end(), AnalysisIn.begin(), AnalysisIn.end());
    std::sort(Cells.begin(), Cells.end(),
              [](const auto &Lhs, const auto &Rhs) { return Lhs.first < Rhs.first; });
    for (const auto &Cell : Cells) {
      n_t Node = Cell.first;
      const auto &Facts = Cell.second;
      OS << "Instruction: ";
      if (Node != nullptr) {
        OS << *Node;
      } else {
        OS << "<null>";
      }
      OS << "\nFacts: ";
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
  const IntraCFG *selectCFG() {
    if (auto *Provided = Problem.getCFG()) {
      return Provided;
    }
    static LLVMIntraCFG DefaultCFG;
    return &DefaultCFG;
  }

  void initialize() {
    std::vector<llvm::Function *> EntryFunctions = Problem.getEntryPoints();
    if (EntryFunctions.empty()) {
      if (auto *M = Problem.getProjectIRDB()) {
        for (const auto &Name : Problem.getEntryPointNames()) {
          if (auto *F = M->getFunction(Name)) {
            EntryFunctions.push_back(F);
          }
        }
      }
    }

    for (auto *Function : EntryFunctions) {
      if (Function == nullptr || Function->isDeclaration()) {
        continue;
      }

      auto Edges = CFG->getAllControlFlowEdges(Function, Problem.direction());
      Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());

      for (auto *Inst : CFG->getAllInstructionsOf(Function)) {
        AnalysisIn.insert({Inst, Problem.allTop()});
      }
    }

    for (const auto &Entry : Problem.initialSeeds()) {
      AnalysisIn[Entry.first] = Entry.second;
    }
  }

  ProblemTy &Problem;
  const IntraCFG *CFG;
  std::deque<std::pair<n_t, n_t>> Worklist;
  std::unordered_map<n_t, mono_container_t> AnalysisIn;
  std::unordered_map<n_t, mono_container_t> AnalysisOut;
  mono_container_t DefaultValue{};
};

template <typename Problem>
using IntraMonoSolver_P =
    IntraMonoSolver<typename Problem::ProblemAnalysisDomain>;

} // namespace mono

#endif // ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_
