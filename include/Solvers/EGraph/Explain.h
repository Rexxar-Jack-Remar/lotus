#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/Subst.h"

namespace lotus::egraph {

template <typename L> struct FlatTerm {
  RecExpr<L> expr;
  std::optional<std::string> backward_rule;
  std::optional<std::string> forward_rule;
  std::vector<FlatTerm<L>> children;

  static FlatTerm fromExpr(const RecExpr<L> &expr, Id id) {
    const auto &node = expr[id];
    FlatTerm term;
    term.expr.add(node.mapChildren([&](Id) { return Id::fromIndex(0); }));
    for (Id child : node.children()) {
      term.children.push_back(fromExpr(expr, child));
    }
    return term;
  }

  std::string toString() const { return expr.toString(); }
};

template <typename L> struct TreeTerm {
  RecExpr<L> expr;
  std::optional<std::string> backward_rule;
  std::optional<std::string> forward_rule;
  std::vector<std::vector<TreeTerm<L>>> child_proofs;
};

template <typename L> class Explanation {
public:
  using TreeExplanation = std::vector<TreeTerm<L>>;
  using FlatExplanation = std::vector<FlatTerm<L>>;

  Explanation() = default;
  explicit Explanation(TreeExplanation explanation_trees)
      : explanation_trees_(std::move(explanation_trees)) {}

  const TreeExplanation &explanationTrees() const { return explanation_trees_; }

  const FlatExplanation &makeFlatExplanation() const {
    if (!flat_explanation_) {
      flat_explanation_ = flattenProof(explanation_trees_);
    }
    return *flat_explanation_;
  }

  std::vector<std::string> getFlatStrings() const {
    std::vector<std::string> out;
    for (const auto &term : makeFlatExplanation()) {
      out.push_back(formatFlat(term));
    }
    if (out.empty()) {
      out.push_back("(Explanation)");
    }
    return out;
  }

  std::string getFlatString() const { return joinStrings(getFlatStrings(), "\n"); }

  std::string getString() const { return formatTrees(explanation_trees_); }
  std::string getStringWithLet() const { return getString(); }

  size_t getTreeSize() const {
    size_t total = 0;
    for (const auto &tree : explanation_trees_) {
      total += treeSize(tree);
    }
    return total;
  }

private:
  static size_t treeSize(const TreeTerm<L> &tree) {
    size_t total = (tree.backward_rule || tree.forward_rule) ? 1u : 0u;
    for (const auto &proofs : tree.child_proofs) {
      for (const auto &child : proofs) {
        total += treeSize(child);
      }
    }
    return total;
  }

  static std::string formatTrees(const TreeExplanation &trees) {
    if (trees.empty()) {
      return "(Explanation)";
    }
    std::vector<std::string> parts;
    for (const auto &tree : trees) {
      parts.push_back(formatTree(tree));
    }
    return joinStrings(parts, "\n");
  }

  static std::string formatTree(const TreeTerm<L> &tree) {
    std::string out = tree.expr.toString();
    if (tree.forward_rule) {
      out = "(Rewrite=> " + *tree.forward_rule + " " + out + ")";
    } else if (tree.backward_rule) {
      out = "(Rewrite<= " + *tree.backward_rule + " " + out + ")";
    }
    if (!tree.child_proofs.empty()) {
      std::vector<std::string> children;
      for (const auto &proofs : tree.child_proofs) {
        children.push_back("(Explanation " + formatTrees(proofs) + ")");
      }
      return "(Explanation " + out + " " + joinStrings(children, " ") + ")";
    }
    return out;
  }

  static std::string formatFlat(const FlatTerm<L> &term) {
    std::string out = term.expr.toString();
    if (term.forward_rule) {
      return "(Rewrite=> " + *term.forward_rule + " " + out + ")";
    }
    if (term.backward_rule) {
      return "(Rewrite<= " + *term.backward_rule + " " + out + ")";
    }
    return out;
  }

  static FlatExplanation flattenProof(const TreeExplanation &trees) {
    FlatExplanation flat;
    for (const auto &tree : trees) {
      flattenTree(tree, flat);
    }
    return flat;
  }

  static void flattenTree(const TreeTerm<L> &tree, FlatExplanation &out) {
    FlatTerm<L> term;
    term.expr = tree.expr;
    term.backward_rule = tree.backward_rule;
    term.forward_rule = tree.forward_rule;
    for (const auto &proofs : tree.child_proofs) {
      for (const auto &child : proofs) {
        flattenTree(child, out);
      }
    }
    out.push_back(std::move(term));
  }

  TreeExplanation explanation_trees_;
  mutable std::optional<FlatExplanation> flat_explanation_;
};

template <typename L, typename A>
inline std::optional<Explanation<L>> explainEquivalence(const EGraph<L, A> &egraph,
                                                        Id lhs, Id rhs);

template <typename L, typename A>
inline std::optional<Explanation<L>> explainEquivalence(const EGraph<L, A> &egraph,
                                                        const RecExpr<L> &lhs,
                                                        const RecExpr<L> &rhs) {
  auto left_ids = egraph.lookupExprUncanonicalIds(lhs);
  auto right_ids = egraph.lookupExprUncanonicalIds(rhs);
  auto left = left_ids && !left_ids->empty() ? std::optional<Id>(left_ids->back()) : std::nullopt;
  auto right =
      right_ids && !right_ids->empty() ? std::optional<Id>(right_ids->back()) : std::nullopt;
  if (!left || !right) {
    return std::nullopt;
  }
  return explainEquivalence(egraph, *left, *right);
}

template <typename L, typename A>
inline std::optional<Explanation<L>> explainMatches(const EGraph<L, A> &egraph,
                                                    const RecExpr<L> &left,
                                                    const PatternAst<L> &right,
                                                    const Subst &subst) {
  Pattern<L> rhs(right);
  EGraph<L, A> clone = egraph;
  Id left_id = clone.lookupExprUncanonicalIds(left)->back();
  Id right_id = clone.addInstantiation(right, subst);
  clone.unite(left_id, right_id, "match");
  clone.rebuild();
  return explainEquivalence(clone, left_id, right_id);
}

template <typename L, typename A>
inline std::optional<Explanation<L>> explainEquivalence(const EGraph<L, A> &egraph,
                                                        Id lhs, Id rhs) {
  if (egraph.find(lhs) != egraph.find(rhs)) {
    return std::nullopt;
  }

  if (lhs == rhs) {
    TreeTerm<L> tree{egraph.originalExpr(lhs), std::nullopt, std::nullopt, {}};
    return Explanation<L>(typename Explanation<L>::TreeExplanation{std::move(tree)});
  }

  struct Edge {
    Id next;
    size_t event_index = 0;
  };

  std::unordered_map<Id, std::vector<Edge>> graph;
  const auto &events = egraph.unionEvents();
  for (size_t i = 0; i < events.size(); ++i) {
    graph[events[i].left].push_back({events[i].right, i});
    graph[events[i].right].push_back({events[i].left, i});
  }

  std::queue<Id> worklist;
  std::unordered_set<Id> visited;
  std::unordered_map<Id, std::pair<Id, size_t>> previous;

  visited.insert(lhs);
  worklist.push(lhs);

  bool found = false;
  while (!worklist.empty() && !found) {
    Id current = worklist.front();
    worklist.pop();

    for (const auto &edge : graph[current]) {
      if (!visited.insert(edge.next).second) {
        continue;
      }
      previous[edge.next] = {current, edge.event_index};
      if (edge.next == rhs) {
        found = true;
        break;
      }
      worklist.push(edge.next);
    }
  }

  if (!found) {
    return std::nullopt;
  }

  using TreeExplanation = typename Explanation<L>::TreeExplanation;
  TreeExplanation trees;
  for (Id current = rhs; current != lhs; current = previous[current].first) {
    const auto &event = events[previous[current].second];
    TreeTerm<L> tree;
    tree.expr = egraph.originalExpr(current);
    if (event.rewrite) {
      tree.forward_rule = event.reason;
    } else {
      tree.backward_rule = event.reason.empty() ? std::optional<std::string>("congruence")
                                                : std::optional<std::string>(event.reason);
    }
    trees.push_back(std::move(tree));
  }
  std::reverse(trees.begin(), trees.end());
  if (trees.empty()) {
    trees.push_back(TreeTerm<L>{egraph.originalExpr(lhs), std::nullopt, std::nullopt, {}});
  }
  return Explanation<L>(std::move(trees));
}

} // namespace lotus::egraph
