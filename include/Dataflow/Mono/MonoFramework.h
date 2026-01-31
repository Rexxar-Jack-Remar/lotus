#ifndef ANALYSIS_MONO_MONOFRAMEWORK_H_
#define ANALYSIS_MONO_MONOFRAMEWORK_H_

// FlowDirection
namespace mono {

enum class FlowDirection {
  Forward,
  Backward,
};

} // namespace mono

// LLVMAnalysisDomain
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

namespace mono {

template <typename ContainerT> struct LLVMMonoAnalysisDomain {
  using n_t = llvm::Instruction *;
  using d_t = llvm::Value *;
  using f_t = llvm::Function *;
  using mono_container_t = ContainerT;
};

} // namespace mono

// IntraMonoProblem
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mono {

template <typename AnalysisDomainTy> class IntraMonoProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;

  explicit IntraMonoProblem(std::vector<llvm::Function *> EntryPoints = {})
      : EntryPoints(std::move(EntryPoints)) {}

  virtual ~IntraMonoProblem() = default;

  virtual mono_container_t normalFlow(n_t Inst,
                                      const mono_container_t &In) = 0;
  virtual mono_container_t merge(const mono_container_t &Lhs,
                                 const mono_container_t &Rhs) = 0;
  virtual bool equal_to(const mono_container_t &Lhs,
                        const mono_container_t &Rhs) = 0;

  virtual mono_container_t allTop() { return mono_container_t{}; }
  virtual std::unordered_map<n_t, mono_container_t> initialSeeds() = 0;
  virtual FlowDirection direction() const { return FlowDirection::Forward; }

  virtual void printContainer(llvm::raw_ostream &,
                              const mono_container_t &) const {}

  [[nodiscard]] const std::vector<llvm::Function *> &getEntryPoints() const {
    return EntryPoints;
  }

protected:
  std::vector<llvm::Function *> EntryPoints;
};

} // namespace mono

// InterMonoProblem
#include "llvm/ADT/ArrayRef.h"

namespace mono {

template <typename AnalysisDomainTy>
class InterMonoProblem : public IntraMonoProblem<AnalysisDomainTy> {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;

  explicit InterMonoProblem(std::vector<llvm::Function *> EntryPoints = {})
      : IntraMonoProblem<AnalysisDomainTy>(std::move(EntryPoints)) {}

  virtual mono_container_t callFlow(n_t CallSite, f_t Callee,
                                    const mono_container_t &In) = 0;
  virtual mono_container_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt,
                                      n_t RetSite,
                                      const mono_container_t &In) = 0;
  virtual mono_container_t callToRetFlow(
      n_t CallSite, n_t RetSite, llvm::ArrayRef<f_t> Callees,
      const mono_container_t &In) = 0;
};

} // namespace mono

// CallStringCTX (global namespace - used by dataflow::)
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <functional>
#include <initializer_list>
#include <stdexcept>

template <typename N, unsigned K> class CallStringCTX {
protected:
  std::deque<N> CallString;
  static constexpr unsigned KLimit = K;
  friend struct std::hash<CallStringCTX<N, K>>;

public:
  CallStringCTX() = default;

  CallStringCTX(std::initializer_list<N> IList) : CallString(IList) {
    if (IList.size() > KLimit) {
      throw std::runtime_error(
          "initial call std::string length exceeds maximal length K");
    }
  }

  void push_back(N Stmt) { // NOLINT
    if (CallString.size() > KLimit - 1) {
      CallString.pop_front();
    }
    CallString.push_back(Stmt);
  }

  N pop_back() { // NOLINT
    if (!CallString.empty()) {
      N Stmt = CallString.back();
      CallString.pop_back();
      return Stmt;
    }
    return N{};
  }

  [[nodiscard]] bool isEqual(const CallStringCTX &Rhs) const {
    return CallString == Rhs.CallString;
  }

  [[nodiscard]] bool isDifferent(const CallStringCTX &Rhs) const {
    return !isEqual(Rhs);
  }

  friend bool operator==(const CallStringCTX<N, K> &Lhs,
                         const CallStringCTX<N, K> &Rhs) {
    return Lhs.isEqual(Rhs);
  }

  friend bool operator!=(const CallStringCTX<N, K> &Lhs,
                         const CallStringCTX<N, K> &Rhs) {
    return !Lhs.isEqual(Rhs);
  }

  friend bool operator<(const CallStringCTX<N, K> &Lhs,
                        const CallStringCTX<N, K> &Rhs) {
    return Lhs.CallString < Rhs.CallString;
  }

  llvm::raw_ostream &print(llvm::raw_ostream &OS) const {
    OS << "Call string: [ ";
    for (auto C : CallString) {
      OS << NToString(C);
      if (C != CallString.back()) {
        OS << " * ";
      }
    }
    return OS << " ]";
  }

  [[nodiscard]] bool empty() const { return CallString.empty(); }

  [[nodiscard]] std::size_t size() const { return CallString.size(); }
};

namespace std {

template <typename N, unsigned K> struct hash<CallStringCTX<N, K>> {
  size_t operator()(const CallStringCTX<N, K> &CS) const noexcept {
    std::hash<unsigned> HashUnsigned;
    llvm::hash_code CallStringHash =
        llvm::hash_combine_range(CS.CallString.begin(), CS.CallString.end());
    llvm::hash_code Combined =
        llvm::hash_combine(HashUnsigned(K), CallStringHash);
    return static_cast<size_t>(Combined);
  }
};

} // namespace std

// IntraMonoSolver
#include "llvm/IR/CFG.h"

#include <deque>
#include <utility>

namespace mono {

template <typename AnalysisDomainTy> class IntraMonoSolver {
public:
  using ProblemTy = IntraMonoProblem<AnalysisDomainTy>;
  using n_t = typename AnalysisDomainTy::n_t;
  using mono_container_t = typename AnalysisDomainTy::mono_container_t;

  explicit IntraMonoSolver(ProblemTy &Problem) : Problem(Problem) {}

  void solve() {
    initialize();
    while (!Worklist.empty()) {
      auto Edge = Worklist.front();
      Worklist.pop_front();
      auto Src = Edge.first;
      auto Dst = Edge.second;

      auto Out = Problem.normalFlow(Src, AnalysisIn[Src]);
      if (isBranchTarget(Dst)) {
        for (auto Pred : getPredsOf(Dst)) {
          if (Pred == Src) {
            continue;
          }
          auto OtherOut = Problem.normalFlow(Pred, AnalysisIn[Pred]);
          Out = Problem.merge(Out, OtherOut);
        }
      }

      if (!Problem.equal_to(Out, AnalysisIn[Dst])) {
        AnalysisIn[Dst] = Out;
        for (auto Succ : getSuccsOf(Dst)) {
          Worklist.push_back({Dst, Succ});
        }
      }
    }

    for (auto &Entry : AnalysisIn) {
      AnalysisOut[Entry.first] = Problem.normalFlow(Entry.first, Entry.second);
    }
  }

  [[nodiscard]] const mono_container_t &getInResultsAt(n_t Stmt) const {
    auto It = AnalysisIn.find(Stmt);
    if (It != AnalysisIn.end()) {
      return It->second;
    }
    return DefaultValue;
  }

  [[nodiscard]] const mono_container_t &getOutResultsAt(n_t Stmt) const {
    auto It = AnalysisOut.find(Stmt);
    if (It != AnalysisOut.end()) {
      return It->second;
    }
    return DefaultValue;
  }

  [[nodiscard]] const std::unordered_map<n_t, mono_container_t> &
  getInResults() const {
    return AnalysisIn;
  }

  [[nodiscard]] const std::unordered_map<n_t, mono_container_t> &
  getOutResults() const {
    return AnalysisOut;
  }

private:
  void initialize() {
    for (auto *Function : Problem.getEntryPoints()) {
      if (Function == nullptr || Function->isDeclaration()) {
        continue;
      }

      auto Edges = getAllControlFlowEdges(Function);
      Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());

      for (auto &BB : *Function) {
        for (auto &Inst : BB) {
          AnalysisIn.insert({&Inst, Problem.allTop()});
        }
      }
    }

    for (const auto &Entry : Problem.initialSeeds()) {
      AnalysisIn[Entry.first] = Entry.second;
    }
  }

  std::vector<std::pair<n_t, n_t>>
  getAllControlFlowEdges(llvm::Function *Function) const {
    std::vector<std::pair<n_t, n_t>> Edges;
    for (auto &BB : *Function) {
      for (auto &Inst : BB) {
        for (auto *Succ : getSuccsOf(&Inst)) {
          Edges.push_back({&Inst, Succ});
        }
      }
    }
    return Edges;
  }

  std::vector<n_t> getSuccsOf(n_t Inst) const {
    return Problem.direction() == FlowDirection::Forward
               ? getForwardSuccs(Inst)
               : getBackwardSuccs(Inst);
  }

  std::vector<n_t> getPredsOf(n_t Inst) const {
    return Problem.direction() == FlowDirection::Forward
               ? getBackwardSuccs(Inst)
               : getForwardSuccs(Inst);
  }

  static std::vector<n_t> getForwardSuccs(n_t Inst) {
    std::vector<n_t> Succs;
    if (Inst->isTerminator()) {
      for (auto *SuccBB : llvm::successors(Inst->getParent())) {
        Succs.push_back(&*SuccBB->begin());
      }
      return Succs;
    }
    if (auto *Next = Inst->getNextNode()) {
      Succs.push_back(Next);
    }
    return Succs;
  }

  static std::vector<n_t> getBackwardSuccs(n_t Inst) {
    std::vector<n_t> Preds;
    auto *BB = Inst->getParent();
    if (Inst != &*BB->begin()) {
      Preds.push_back(Inst->getPrevNode());
      return Preds;
    }
    for (auto *PredBB : llvm::predecessors(BB)) {
      Preds.push_back(PredBB->getTerminator());
    }
    return Preds;
  }

  bool isBranchTarget(n_t Inst) const { return getPredsOf(Inst).size() > 1; }

  ProblemTy &Problem;
  std::deque<std::pair<n_t, n_t>> Worklist;
  std::unordered_map<n_t, mono_container_t> AnalysisIn;
  std::unordered_map<n_t, mono_container_t> AnalysisOut;
  mono_container_t DefaultValue{};
};

} // namespace mono

#endif // ANALYSIS_MONO_MONOFRAMEWORK_H_
