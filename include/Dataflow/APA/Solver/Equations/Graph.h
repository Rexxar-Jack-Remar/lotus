#pragma once

#include "Dataflow/APA/Core/PathExpr.h"

#include <map>
#include <utility>
#include <vector>

namespace elimination {

// A graph of left-linear algebraic summary equations:
//
//   X_u = base_u U (W_u,v . X_v)
//
// Nodes are intended to represent interprocedural summary instances such as
// (function, call-string context). Edge weights are APA path expressions, not
// ordinary scheduler dependencies. The solver computes the closed-form regular
// path-expression solution by solving SCCs in dependency order.
template <typename KeyT, typename TransferT>
class PathSummaryEquationGraph final {
public:
  using key_t = KeyT;
  using transfer_t = TransferT;
  using expr_factory_t = PathExprFactory<TransferT>;
  using expr_ref_t = typename expr_factory_t::Ref;

  struct Node final {
    key_t Key;
    expr_ref_t Base;
  };

  struct Edge final {
    std::size_t Source = 0;
    std::size_t Target = 0;
    expr_ref_t Weight;
  };

  std::size_t addNode(key_t Key, expr_ref_t Base = {}) {
    auto It = Index.find(Key);
    if (It != Index.end()) {
      if (Base) {
        Nodes[It->second].Base = Base;
      }
      return It->second;
    }
    const std::size_t Id = Nodes.size();
    if (!Base) {
      Base = Exprs.zero();
    }
    Nodes.push_back(Node{std::move(Key), Base});
    Index.emplace(Nodes.back().Key, Id);
    return Id;
  }

  void setBase(const key_t &Key, expr_ref_t Base) {
    Nodes[addNode(Key)].Base = Base ? Base : Exprs.zero();
  }

  void addEdge(const key_t &Source, const key_t &Target, expr_ref_t Weight) {
    const std::size_t SourceId = addNode(Source);
    const std::size_t TargetId = addNode(Target);
    Edges.push_back(Edge{SourceId, TargetId, Weight ? Weight : Exprs.one()});
  }

  const std::vector<Node> &nodes() const { return Nodes; }
  const std::vector<Edge> &edges() const { return Edges; }

  const expr_factory_t &exprs() const { return Exprs; }
  expr_factory_t &exprs() { return Exprs; }

private:
  expr_factory_t Exprs;
  std::vector<Node> Nodes;
  std::vector<Edge> Edges;
  std::map<key_t, std::size_t> Index;
};

} // namespace elimination
