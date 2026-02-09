#ifndef ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_
#define ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_

#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/Mono/ControlFlow/IntraCFG.h"
#include "Dataflow/Mono/Debug/MonoDebug.h"
#include "Dataflow/Mono/IntraMonoProblem.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>
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

  void setDebugConfig(const DebugConfig &Config) { DebugCfg = Config; }

  const DebugConfig &getDebugConfig() const { return DebugCfg; }

  const SolverStatistics &getStatistics() const { return Stats; }

  void solve() {
    auto start_time = std::chrono::steady_clock::now();

    MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg,
                        "Starting IntraMonoSolver::solve()");

    initialize();

    auto init_end_time = std::chrono::steady_clock::now();
    Stats.initialization_time =
        std::chrono::duration_cast<std::chrono::microseconds>(init_end_time -
                                                              start_time);

    auto solve_start_time = std::chrono::steady_clock::now();

    while (!Worklist.empty()) {
      Stats.record_worklist_size(Worklist.size());
      Stats.iterations++;

      if (DebugCfg.is_enabled(DebugLevel::Debug) &&
          Stats.iterations <= DebugCfg.max_iterations_log) {
        llvm::outs() << "[WORKLIST] Iteration " << Stats.iterations
                     << ", size=" << Worklist.size() << "\n";
      }

      auto Edge = Worklist.front();
      Worklist.pop_front();
      Stats.worklist_total_pops++;

      auto Src = Edge.first;
      auto Dst = Edge.second;

      MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg,
                          "Processing edge: " << Src << " -> " << Dst);

      Stats.flow_function_calls++;
      auto Out = Problem.normalFlow(Src, AnalysisIn[Src]);

      if (CFG->isBranchTarget(Src, Dst, Problem.direction())) {
        MONO_TRACE_MERGE(llvm::outs(), DebugCfg,
                         "Branch target detected at " << Dst);
        for (auto Pred : CFG->getPredsOf(Dst, Problem.direction())) {
          if (Pred == Src) {
            continue;
          }
          Stats.flow_function_calls++;
          Stats.merge_operations++;
          auto OtherOut = Problem.normalFlow(Pred, AnalysisIn[Pred]);
          Out = Problem.merge(Out, OtherOut);
        }
      }

      Stats.stabilization_checks++;
      if (!Problem.equal_to(Out, AnalysisIn[Dst])) {
        MONO_TRACE_FACTS(llvm::outs(), DebugCfg, "Facts changed at " << Dst);
        AnalysisIn[Dst] = Out;
        for (auto Succ : CFG->getSuccsOf(Dst, Problem.direction())) {
          Worklist.push_back({Dst, Succ});
        }
      }
    }

    for (auto &Entry : AnalysisIn) {
      AnalysisOut[Entry.first] = Problem.normalFlow(Entry.first, Entry.second);
    }

    auto solve_end_time = std::chrono::steady_clock::now();
    Stats.solving_time = std::chrono::duration_cast<std::chrono::microseconds>(
        solve_end_time - solve_start_time);
    Stats.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
        solve_end_time - start_time);

    MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg,
                        "IntraMonoSolver finished after " << Stats.iterations
                                                          << " iterations");

    if (DebugCfg.collect_statistics) {
      Stats.dump(llvm::outs());
    }
  }

  const mono_container_t &getResultsAt(n_t Stmt) const {
    return getInResultsAt(Stmt);
  }

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
    std::sort(Cells.begin(), Cells.end(), [](const auto &Lhs, const auto &Rhs) {
      return Lhs.first < Rhs.first;
    });
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

    std::unordered_set<llvm::Function *> SeenFunctions;
    for (auto *Function : EntryFunctions) {
      if (Function == nullptr || Function->isDeclaration()) {
        continue;
      }
      SeenFunctions.insert(Function);
      auto Edges = CFG->getAllControlFlowEdges(Function, Problem.direction());
      Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());
      for (auto *Inst : CFG->getAllInstructionsOf(Function)) {
        AnalysisIn.insert({Inst, Problem.allTop()});
      }
    }

    // Ensure any function that contains a seed node has its CFG in the
    // worklist so propagation from that seed can occur (correctness when
    // initialSeeds() targets instructions outside EntryPoints).
    auto Seeds = Problem.initialSeeds();
    for (const auto &Entry : Seeds) {
      auto *BB = Entry.first ? Entry.first->getParent() : nullptr;
      auto *F = BB ? BB->getParent() : nullptr;
      if (F && !F->isDeclaration() && SeenFunctions.insert(F).second) {
        auto Edges = CFG->getAllControlFlowEdges(F, Problem.direction());
        Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());
        for (auto *Inst : CFG->getAllInstructionsOf(F)) {
          AnalysisIn.insert({Inst, Problem.allTop()});
        }
      }
    }
    for (const auto &Entry : Seeds) {
      AnalysisIn[Entry.first] = Entry.second;
    }
  }

  ProblemTy &Problem;
  const IntraCFG *CFG;
  std::deque<std::pair<n_t, n_t>> Worklist;
  std::unordered_map<n_t, mono_container_t> AnalysisIn;
  std::unordered_map<n_t, mono_container_t> AnalysisOut;
  mono_container_t DefaultValue{};
  DebugConfig DebugCfg;
  SolverStatistics Stats;
};

template <typename Problem>
using IntraMonoSolver_P =
    IntraMonoSolver<typename Problem::ProblemAnalysisDomain>;

} // namespace mono

#endif // ANALYSIS_MONO_SOLVER_INTRAMONOSOLVER_H_
