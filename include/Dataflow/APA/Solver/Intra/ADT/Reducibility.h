#pragma once

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Solver/Intra/ADT/Types.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elimination::detail {

template <typename AnalysisTypesT> class Reducibility final {
public:
  using ProblemTy = IntraEliminationProblem<AnalysisTypesT>;
  using ReducibleProblemTy = IntraReducibleEliminationProblem<AnalysisTypesT>;
  using n_t = typename ProblemTy::n_t;
  using transfer_t = typename ProblemTy::transfer_t;
  using expr_factory_t = PathExprFactory<transfer_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using Types = ADTTypes<n_t, expr_ref_t>;
  using Edge = typename Types::Edge;
  struct ReducibleViewProvided final {
    const ReducibleProblemTy *R = nullptr;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;

    explicit ReducibleViewProvided(const ReducibleProblemTy &R) : R(&R) {}

    bool init() {
      Topo = R->topologicalOrder();
      if (Topo.empty()) {
        return false;
      }
      Edges.clear();
      const auto REdges = R->edges();
      Edges.reserve(REdges.size());
      for (const auto &E : REdges) {
        Edges.push_back({E.Src, E.Dst});
      }
      return true;
    }

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return R->idom(N); }
    bool dominates(n_t A, n_t B) const { return R->dominates(A, B); }
    bool isBackEdge(n_t Src, n_t Dst) const { return R->isBackEdge(Src, Dst); }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return R->edgeTransfer(Src, Dst);
    }
  };

  // Reducible-graph metadata synthesized from a plain CFG when the client does
  // not implement IntraReducibleEliminationProblem directly.
  struct ComputedReducibleView final {
    const ProblemTy *Problem = nullptr;
    std::vector<n_t> Nodes;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;
    std::unordered_map<n_t, std::size_t> NodeIndex;
    std::vector<std::vector<std::size_t>> Preds;
    std::vector<n_t> Idom;
    std::vector<std::vector<std::size_t>> DomTreeChildren;
    std::vector<std::size_t> DomTin;
    std::vector<std::size_t> DomTout;

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return Idom.at(NodeIndex.at(N)); }
    bool dominates(n_t A, n_t B) const {
      const auto IA = NodeIndex.at(A);
      const auto IB = NodeIndex.at(B);
      return DomTin[IA] <= DomTin[IB] && DomTout[IB] <= DomTout[IA];
    }
    bool isBackEdge(n_t Src, n_t Dst) const { return dominates(Dst, Src); }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return Problem->edgeTransfer(Src, Dst);
    }
  };

  Reducibility(const ProblemTy &Problem, SolveDiagnostics &Diagnostics)
      : Problem(Problem), Diagnostics(Diagnostics) {}

  // Build reducible metadata from a plain CFG.
  //
  // The ADT engines require stronger invariants than state elimination. This
  // routine rejects the ADT path unless all of the following hold:
  //   1) every listed node is reachable from entry,
  //   2) immediate dominators can be computed for all nodes,
  //   3) removing back edges yields an acyclic graph with entry first in topo.
  // When any check fails, the caller falls back to StateElimination.
  bool buildComputedReducibleView(ComputedReducibleView &Out) const {
    Out = ComputedReducibleView{};
    Out.Problem = &Problem;
    Out.Nodes = Problem.nodes();
    if (Out.Nodes.empty()) {
      return rejectADT(ADTRejectionReason::EmptyTopologicalOrder);
    }

    Out.NodeIndex.clear();
    Out.NodeIndex.reserve(Out.Nodes.size());
    for (std::size_t i = 0; i < Out.Nodes.size(); ++i) {
      Out.NodeIndex.emplace(Out.Nodes[i], i);
    }

    const auto Entry = Problem.entry();
    const auto EntryIt = Out.NodeIndex.find(Entry);
    if (EntryIt == Out.NodeIndex.end()) {
      return rejectADT(ADTRejectionReason::MissingTopologicalNode);
    }
    const std::size_t EntryIdx = EntryIt->second;

    // ADT-based methods only make sense on a connected intraprocedural region
    // rooted at entry, so reject problems with missing entry reachability.
    std::unordered_set<n_t> Reach;
    Reach.reserve(Out.Nodes.size());
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      const auto Cur = Stack.back();
      Stack.pop_back();
      for (const auto &Succ : Problem.succs(Cur)) {
        if (Out.NodeIndex.find(Succ) == Out.NodeIndex.end()) {
          continue;
        }
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }
    if (Reach.size() != Out.Nodes.size()) {
      return rejectADT(ADTRejectionReason::DisconnectedFromEntry);
    }

    Out.Edges.clear();
    Out.Preds.assign(Out.Nodes.size(), {});
    for (const auto &Src : Out.Nodes) {
      const auto SrcIdx = Out.NodeIndex.at(Src);
      for (const auto &Dst : Problem.succs(Src)) {
        auto It = Out.NodeIndex.find(Dst);
        if (It == Out.NodeIndex.end()) {
          continue;
        }
        const auto DstIdx = It->second;
        Out.Edges.push_back({Src, Dst});
        Out.Preds[DstIdx].push_back(SrcIdx);
      }
    }

    const std::size_t N = Out.Nodes.size();
    std::vector<std::vector<std::size_t>> Succ(N);
    for (const auto &E : Out.Edges) {
      const auto SrcIdx = Out.NodeIndex.at(E.Src);
      const auto DstIdx = Out.NodeIndex.at(E.Dst);
      Succ[SrcIdx].push_back(DstIdx);
    }

    // Compute reverse postorder numbers from entry for iterative idom.
    std::vector<std::size_t> PostOrder;
    PostOrder.reserve(N);
    std::vector<std::size_t> NextSucc(N, 0);
    std::vector<std::size_t> DFSStack;
    std::vector<bool> Seen(N, false);
    DFSStack.push_back(EntryIdx);
    Seen[EntryIdx] = true;
    while (!DFSStack.empty()) {
      const auto Cur = DFSStack.back();
      auto &SI = NextSucc[Cur];
      if (SI < Succ[Cur].size()) {
        const auto Dst = Succ[Cur][SI++];
        if (!Seen[Dst]) {
          Seen[Dst] = true;
          DFSStack.push_back(Dst);
        }
        continue;
      }
      PostOrder.push_back(Cur);
      DFSStack.pop_back();
    }
    if (PostOrder.size() != N) {
      return rejectADT(ADTRejectionReason::DisconnectedFromEntry);
    }

    std::vector<std::size_t> RPO;
    RPO.reserve(N);
    for (auto It = PostOrder.rbegin(); It != PostOrder.rend(); ++It) {
      RPO.push_back(*It);
    }
    std::vector<std::size_t> Rank(N, 0);
    for (std::size_t I = 0; I < RPO.size(); ++I) {
      Rank[RPO[I]] = I;
    }

    std::vector<int> IdomIdx(N, -1);
    IdomIdx[EntryIdx] = static_cast<int>(EntryIdx);

    auto Intersect = [&](std::size_t A, std::size_t B) {
      while (A != B) {
        while (Rank[A] > Rank[B]) {
          A = static_cast<std::size_t>(IdomIdx[A]);
        }
        while (Rank[B] > Rank[A]) {
          B = static_cast<std::size_t>(IdomIdx[B]);
        }
      }
      return A;
    };

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto V : RPO) {
        if (V == EntryIdx) {
          continue;
        }
        std::size_t NewIdom = static_cast<std::size_t>(-1);
        for (const auto P : Out.Preds[V]) {
          if (IdomIdx[P] == -1) {
            continue;
          }
          if (NewIdom == static_cast<std::size_t>(-1)) {
            NewIdom = P;
          } else {
            NewIdom = Intersect(P, NewIdom);
          }
        }
        if (NewIdom == static_cast<std::size_t>(-1)) {
          return rejectADT(ADTRejectionReason::InvalidImmediateDominator);
        }
        if (IdomIdx[V] != static_cast<int>(NewIdom)) {
          IdomIdx[V] = static_cast<int>(NewIdom);
          Changed = true;
        }
      }
    }

    Out.Idom.assign(N, Out.Nodes.front());
    for (std::size_t V = 0; V < N; ++V) {
      if (IdomIdx[V] == -1) {
        return rejectADT(ADTRejectionReason::InvalidImmediateDominator);
      }
      Out.Idom[V] = Out.Nodes[static_cast<std::size_t>(IdomIdx[V])];
    }

    Out.DomTreeChildren.assign(N, {});
    for (std::size_t V = 0; V < N; ++V) {
      if (V == EntryIdx) {
        continue;
      }
      Out.DomTreeChildren[static_cast<std::size_t>(IdomIdx[V])].push_back(V);
    }
    Out.DomTin.assign(N, 0);
    Out.DomTout.assign(N, 0);
    std::size_t Time = 0;
    struct DomFrame {
      std::size_t Node = 0;
      std::size_t ChildIdx = 0;
    };
    std::vector<DomFrame> DomStack;
    DomStack.push_back({EntryIdx, 0});
    while (!DomStack.empty()) {
      auto &Top = DomStack.back();
      if (Top.ChildIdx == 0) {
        Out.DomTin[Top.Node] = Time++;
      }
      if (Top.ChildIdx < Out.DomTreeChildren[Top.Node].size()) {
        const auto Child = Out.DomTreeChildren[Top.Node][Top.ChildIdx++];
        DomStack.push_back({Child, 0});
        continue;
      }
      Out.DomTout[Top.Node] = Time++;
      DomStack.pop_back();
    }

    // Topologically sort only the non-back edges. Failure here means the graph
    // violates reducibility assumptions required by the ADT engines.
    std::vector<std::vector<std::size_t>> SuccF(N);
    std::vector<std::size_t> InDeg(N, 0);
    for (const auto &E : Out.Edges) {
      const auto SrcIdx = Out.NodeIndex.at(E.Src);
      const auto DstIdx = Out.NodeIndex.at(E.Dst);
      if (Out.dominates(E.Dst, E.Src)) {
        continue;
      }
      SuccF[SrcIdx].push_back(DstIdx);
      ++InDeg[DstIdx];
    }

    if (InDeg[EntryIdx] != 0) {
      return rejectADT(ADTRejectionReason::NonBackEdgeCycle);
    }

    std::deque<std::size_t> Ready;
    Ready.push_back(EntryIdx);
    for (std::size_t i = 0; i < N; ++i) {
      if (i == EntryIdx) {
        continue;
      }
      if (InDeg[i] == 0) {
        Ready.push_back(i);
      }
    }

    Out.Topo.clear();
    Out.Topo.reserve(N);
    while (!Ready.empty()) {
      const auto Cur = Ready.front();
      Ready.pop_front();
      Out.Topo.push_back(Out.Nodes[Cur]);
      for (const auto S : SuccF[Cur]) {
        if (--InDeg[S] == 0) {
          Ready.push_back(S);
        }
      }
    }

    if (Out.Topo.size() != N) {
      return rejectADT(ADTRejectionReason::NonBackEdgeCycle);
    }
    if (Out.Topo.front() != Entry) {
      return rejectADT(ADTRejectionReason::EntryNotFirst);
    }
    return true;
  }

  bool rejectADT(ADTRejectionReason Reason) const {
    if (Diagnostics.adt_rejection_reason == ADTRejectionReason::None) {
      Diagnostics.adt_rejection_reason = Reason;
    }
    return false;
  }

private:
  const ProblemTy &Problem;
  SolveDiagnostics &Diagnostics;
};

} // namespace elimination::detail
