#ifndef DATAFLOW_APA_SOLVER_CALLGRAPHSCC_H_
#define DATAFLOW_APA_SOLVER_CALLGRAPHSCC_H_

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <vector>

namespace elimination {

// Call graph over procedures (f_t), condensed into strongly-connected
// components in reverse-topological order (callees before callers), for the
// modular interprocedural APA solver (E6). Procedures are discovered from a set
// of entry procedures by walking each procedure's instructions, so the build is
// fully generic over the interprocedural CFG interface: it only uses
//   ICF.getAllInstructionsOf(f_t)  -> instructions of a procedure
//   ICF.isCallSite(n_t)            -> is this a call
//   ICF.getCalleesOfCallAt(n_t)    -> callees at a call site (set; indirect ok)
//   ICF.getStartPointsOf(f_t)      -> entry nodes (empty => opaque/external)
//   ICF.getExitPointsOf(f_t)       -> exit nodes  (empty => opaque/external)
//
// External / declaration-only callees (no start or exit points) are treated as
// opaque: no summary, no call-graph edge (their call sites are handled by the
// call-to-return bypass downstream, matching ForwardInterSummarySolver).
template <typename FunctionT> struct CallGraphComponent final {
  std::vector<FunctionT> procs; // members of this SCC
  bool recursive = false;       // size > 1, or a self-edge
};

template <typename FunctionT> struct CallGraphSCCResult final {
  // Reverse-topological: a component's callees appear before it. A modular
  // solver can therefore compute summaries in a single left-to-right pass.
  std::vector<CallGraphComponent<FunctionT>> order;
};

template <typename NodeT, typename FunctionT, typename ICFTy>
class CallGraphSCCBuilder final {
public:
  using n_t = NodeT;
  using f_t = FunctionT;

  explicit CallGraphSCCBuilder(const ICFTy &ICF) : ICF(ICF) {}

  // Build the condensed call graph reachable from Entries. A procedure with no
  // start or exit points is opaque (external) and is never added as a node.
  CallGraphSCCResult<f_t> build(const std::vector<f_t> &Entries) {
    Procs.clear();
    Index.clear();
    Adj.clear();

    std::deque<f_t> Worklist;
    for (const auto &E : Entries) {
      if (isOpaque(E)) {
        continue;
      }
      if (intern(E).second) {
        Worklist.push_back(E);
      }
    }

    while (!Worklist.empty()) {
      const f_t F = Worklist.front();
      Worklist.pop_front();
      const std::size_t Fi = Index.at(F);
      for (const auto &Inst : ICF.getAllInstructionsOf(F)) {
        if (!ICF.isCallSite(Inst)) {
          continue;
        }
        for (const auto &Callee : ICF.getCalleesOfCallAt(Inst)) {
          if (isOpaque(Callee)) {
            continue; // external/declaration-only: opaque, no edge
          }
          auto [Ci, Fresh] = intern(Callee);
          addEdge(Fi, Ci);
          if (Fresh) {
            Worklist.push_back(Callee);
          }
        }
      }
    }

    return condense();
  }

private:
  bool isOpaque(const f_t &F) const {
    return ICF.getStartPointsOf(F).empty() || ICF.getExitPointsOf(F).empty();
  }

  // Returns (index, freshly-inserted?).
  std::pair<std::size_t, bool> intern(const f_t &F) {
    auto It = Index.find(F);
    if (It != Index.end()) {
      return {It->second, false};
    }
    const std::size_t Id = Procs.size();
    Procs.push_back(F);
    Index.emplace(F, Id);
    Adj.emplace_back();
    return {Id, true};
  }

  void addEdge(std::size_t From, std::size_t To) {
    auto &Row = Adj[From];
    if (std::find(Row.begin(), Row.end(), To) == Row.end()) {
      Row.push_back(To);
    }
  }

  // Iterative Tarjan SCC (explicit stack; no recursion so deep call graphs
  // cannot overflow). Tarjan emits components in reverse-topological order,
  // which is exactly the order the modular solver wants.
  CallGraphSCCResult<f_t> condense() const {
    const std::size_t N = Procs.size();
    CallGraphSCCResult<f_t> Out;

    std::vector<int> Idx(N, -1);
    std::vector<int> Low(N, 0);
    std::vector<bool> OnStack(N, false);
    std::vector<std::size_t> Stack;
    std::vector<std::size_t> CompOf(N, static_cast<std::size_t>(-1));
    int Next = 0;

    // Explicit DFS frame: node + position in its adjacency list.
    struct Frame final {
      std::size_t V;
      std::size_t Edge;
    };
    std::vector<Frame> Work;

    for (std::size_t Root = 0; Root < N; ++Root) {
      if (Idx[Root] != -1) {
        continue;
      }
      Work.push_back({Root, 0});
      while (!Work.empty()) {
        auto &Fr = Work.back();
        const std::size_t V = Fr.V;
        if (Fr.Edge == 0 && Idx[V] == -1) {
          Idx[V] = Low[V] = Next++;
          Stack.push_back(V);
          OnStack[V] = true;
        }
        if (Fr.Edge < Adj[V].size()) {
          const std::size_t W = Adj[V][Fr.Edge++];
          if (Idx[W] == -1) {
            Work.push_back({W, 0});
          } else if (OnStack[W]) {
            Low[V] = std::min(Low[V], Idx[W]);
          }
          continue;
        }
        // All successors processed: settle V.
        if (Low[V] == Idx[V]) {
          CallGraphComponent<f_t> C;
          bool SelfEdge = false;
          while (true) {
            const std::size_t W = Stack.back();
            Stack.pop_back();
            OnStack[W] = false;
            CompOf[W] = Out.order.size();
            C.procs.push_back(Procs[W]);
            if (W == V) {
              break;
            }
          }
          // Self-edge detection for a singleton (direct recursion).
          if (C.procs.size() == 1) {
            for (std::size_t W : Adj[V]) {
              if (W == V) {
                SelfEdge = true;
                break;
              }
            }
          }
          C.recursive = C.procs.size() > 1 || SelfEdge;
          Out.order.push_back(std::move(C));
        }
        const std::size_t Settled = V;
        Work.pop_back();
        if (!Work.empty()) {
          Low[Work.back().V] = std::min(Low[Work.back().V], Low[Settled]);
        }
      }
    }
    return Out;
  }

  const ICFTy &ICF;
  std::vector<f_t> Procs;
  std::unordered_map<f_t, std::size_t> Index;
  std::vector<std::vector<std::size_t>> Adj;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_CALLGRAPHSCC_H_
