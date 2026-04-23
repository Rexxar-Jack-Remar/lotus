#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/Subst.h"

#include <memory>
#include <queue>

namespace lotus::egraph {

template <typename L, typename A> class Rewrite;

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
  std::vector<std::vector<std::shared_ptr<TreeTerm<L>>>> child_proofs;
  Id last = Id::fromIndex(0);
  Id current = Id::fromIndex(0);
};

template <typename L> class Explanation {
public:
  using TreeExplanation = std::vector<std::shared_ptr<TreeTerm<L>>>;
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

  std::string getFlatString() const {
    return joinStrings(getFlatStrings(), "\n");
  }

  std::string getString() const { return formatTrees(explanation_trees_); }
  std::string getStringWithLet() const {
    std::unordered_map<std::string, size_t> counts;
    for (const auto &tree : explanation_trees_) {
      collectFingerprints(*tree, counts);
    }

    std::unordered_map<std::string, std::string> bindings;
    std::vector<std::pair<std::string, const TreeTerm<L> *>> ordered_bindings;
    for (const auto &tree : explanation_trees_) {
      assignBindings(*tree, counts, bindings, ordered_bindings);
    }

    std::vector<std::string> parts;
    for (const auto &tree : explanation_trees_) {
      parts.push_back(formatTreeWithBindings(*tree, bindings, true));
    }
    std::string result =
        parts.empty() ? "(Explanation)" : joinStrings(parts, "\n");
    for (auto it = ordered_bindings.rbegin(); it != ordered_bindings.rend();
         ++it) {
      result = "(let (" + it->first + " " +
               formatTreeWithBindings(*it->second, bindings, true) + ") " +
               result + ")";
    }
    return result;
  }

  size_t getTreeSize() const {
    std::unordered_set<const TreeTerm<L> *> seen;
    std::unordered_set<std::pair<Id, Id>, PairHash> seen_adjacent;
    size_t total = 0;
    for (const auto &tree : explanation_trees_) {
      total += treeSize(seen, seen_adjacent, tree);
    }
    return total;
  }

private:
  static size_t
  treeSize(std::unordered_set<const TreeTerm<L> *> &seen,
           std::unordered_set<std::pair<Id, Id>, PairHash> &seen_adjacent,
           const std::shared_ptr<TreeTerm<L>> &tree) {
    if (!seen.insert(tree.get()).second) {
      return 0;
    }
    size_t total = (tree->backward_rule || tree->forward_rule) ? 1u : 0u;
    if (total == 1) {
      auto edge = std::make_pair(tree->current, tree->last);
      if (!seen_adjacent.insert(edge).second) {
        return 0;
      }
      seen_adjacent.insert({tree->last, tree->current});
    }
    for (const auto &proofs : tree->child_proofs) {
      for (const auto &child : proofs) {
        total += treeSize(seen, seen_adjacent, child);
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
      parts.push_back(formatTree(*tree));
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

  static std::string fingerprint(const TreeTerm<L> &tree) {
    std::vector<std::string> children;
    for (const auto &proofs : tree.child_proofs) {
      std::vector<std::string> nested;
      for (const auto &child : proofs) {
        nested.push_back(fingerprint(*child));
      }
      children.push_back("[" + joinStrings(nested, ",") + "]");
    }
    return tree.expr.toString() +
           "|f=" + (tree.forward_rule ? *tree.forward_rule : "") +
           "|b=" + (tree.backward_rule ? *tree.backward_rule : "") +
           "|c=" + joinStrings(children, ";");
  }

  static void
  collectFingerprints(const TreeTerm<L> &tree,
                      std::unordered_map<std::string, size_t> &counts) {
    std::string key = fingerprint(tree);
    ++counts[key];
    for (const auto &proofs : tree.child_proofs) {
      for (const auto &child : proofs) {
        collectFingerprints(*child, counts);
      }
    }
  }

  static void
  assignBindings(const TreeTerm<L> &tree,
                 const std::unordered_map<std::string, size_t> &counts,
                 std::unordered_map<std::string, std::string> &bindings,
                 std::vector<std::pair<std::string, const TreeTerm<L> *>>
                     &ordered_bindings) {
    for (const auto &proofs : tree.child_proofs) {
      for (const auto &child : proofs) {
        assignBindings(*child, counts, bindings, ordered_bindings);
      }
    }

    std::string key = fingerprint(tree);
    auto count_it = counts.find(key);
    if (count_it != counts.end() && count_it->second > 1 &&
        !bindings.count(key)) {
      std::string name = "v_" + std::to_string(bindings.size());
      bindings.emplace(key, name);
      ordered_bindings.emplace_back(name, &tree);
    }
  }

  static std::string formatTreeWithBindings(
      const TreeTerm<L> &tree,
      const std::unordered_map<std::string, std::string> &bindings,
      bool is_root = false) {
    std::string key = fingerprint(tree);
    if (!is_root) {
      if (auto it = bindings.find(key); it != bindings.end()) {
        return it->second;
      }
    }

    std::string out = tree.expr.toString();
    if (tree.forward_rule) {
      out = "(Rewrite=> " + *tree.forward_rule + " " + out + ")";
    } else if (tree.backward_rule) {
      out = "(Rewrite<= " + *tree.backward_rule + " " + out + ")";
    }
    if (!tree.child_proofs.empty()) {
      std::vector<std::string> children;
      for (const auto &proofs : tree.child_proofs) {
        std::vector<std::string> nested;
        for (const auto &child : proofs) {
          nested.push_back(formatTreeWithBindings(*child, bindings));
        }
        children.push_back("(Explanation " + joinStrings(nested, "\n") + ")");
      }
      out = "(Explanation " + out + " " + joinStrings(children, " ") + ")";
    }
    return out;
  }

  static FlatExplanation flattenProof(const TreeExplanation &trees) {
    FlatExplanation flat;
    for (const auto &tree : trees) {
      flattenTree(*tree, flat);
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
inline std::optional<Explanation<L>>
explainEquivalence(const EGraph<L, A> &egraph, Id lhs, Id rhs);

namespace detail {

struct PathEntry {
  size_t cost = std::numeric_limits<size_t>::max();
  std::vector<ExplanationConnection> path;
};

using PathMemo = std::unordered_map<std::pair<Id, Id>, PathEntry, PairHash>;
using ActivePairs = std::unordered_set<std::pair<Id, Id>, PairHash>;

template <typename L, typename A>
inline std::optional<std::vector<ExplanationConnection>>
findExplanationPath(const EGraph<L, A> &egraph, Id lhs, Id rhs) {
  if (lhs == rhs) {
    return std::vector<ExplanationConnection>{};
  }

  std::queue<Id> worklist;
  std::unordered_set<Id> visited;
  std::unordered_map<Id, std::pair<Id, ExplanationConnection>> previous;

  visited.insert(lhs);
  worklist.push(lhs);

  const auto &nodes = egraph.explanationNodes();
  while (!worklist.empty()) {
    Id current = worklist.front();
    worklist.pop();

    if (current.index() >= nodes.size()) {
      continue;
    }

    for (const auto &edge : nodes[current.index()].neighbors) {
      if (!visited.insert(edge.next).second) {
        continue;
      }
      previous.emplace(edge.next, std::make_pair(current, edge));
      if (edge.next == rhs) {
        std::vector<ExplanationConnection> path;
        for (Id cursor = rhs; cursor != lhs;
             cursor = previous.at(cursor).first) {
          path.push_back(previous.at(cursor).second);
        }
        std::reverse(path.begin(), path.end());
        return path;
      }
      worklist.push(edge.next);
    }
  }

  return std::nullopt;
}

template <typename L, typename A>
inline Id commonAncestor(const EGraph<L, A> &egraph, Id lhs, Id rhs) {
  std::unordered_set<Id> seen_left;
  std::unordered_set<Id> seen_right;
  const auto &nodes = egraph.explanationNodes();
  while (true) {
    seen_left.insert(lhs);
    if (seen_right.count(lhs)) {
      return lhs;
    }
    seen_right.insert(rhs);
    if (seen_left.count(rhs)) {
      return rhs;
    }
    lhs = nodes.at(lhs.index()).parent_connection.next;
    rhs = nodes.at(rhs.index()).parent_connection.next;
  }
}

template <typename L, typename A>
inline std::vector<ExplanationConnection>
getConnections(const EGraph<L, A> &egraph, Id node, Id ancestor) {
  std::vector<ExplanationConnection> connections;
  const auto &nodes = egraph.explanationNodes();
  while (node != ancestor) {
    const auto &connection = nodes.at(node.index()).parent_connection;
    connections.push_back(connection);
    node = connection.next;
  }
  return connections;
}

template <typename L, typename A>
inline std::optional<std::vector<ExplanationConnection>>
getParentPath(const EGraph<L, A> &egraph, Id lhs, Id rhs) {
  if (lhs == rhs) {
    return std::vector<ExplanationConnection>{};
  }
  Id ancestor = commonAncestor(egraph, lhs, rhs);
  auto left_connections = getConnections(egraph, lhs, ancestor);
  auto right_connections = getConnections(egraph, rhs, ancestor);
  std::vector<ExplanationConnection> path = std::move(left_connections);
  for (auto it = right_connections.rbegin(); it != right_connections.rend();
       ++it) {
    auto edge = *it;
    std::swap(edge.current, edge.next);
    edge.is_rewrite_forward = !edge.is_rewrite_forward;
    path.push_back(std::move(edge));
  }
  return path;
}

template <typename L, typename A>
inline PathEntry shortestExplanationPath(const EGraph<L, A> &egraph, Id lhs,
                                         Id rhs, PathMemo &memo,
                                         ActivePairs &active);

template <typename L, typename A>
inline size_t connectionCost(const EGraph<L, A> &egraph,
                             const ExplanationConnection &connection,
                             PathMemo &memo, ActivePairs &active) {
  if (connection.justification == ExplanationJustificationKind::Rule) {
    return 1;
  }

  const auto &left_node = egraph.originalNode(connection.current);
  const auto &right_node = egraph.originalNode(connection.next);
  if (!left_node.matches(right_node)) {
    return std::numeric_limits<size_t>::max() / 4;
  }

  size_t total = 0;
  auto left_children = left_node.children();
  auto right_children = right_node.children();
  for (size_t i = 0; i < left_children.size() && i < right_children.size();
       ++i) {
    auto child = shortestExplanationPath(egraph, left_children[i],
                                         right_children[i], memo, active);
    if (child.cost >= std::numeric_limits<size_t>::max() / 8) {
      return child.cost;
    }
    if (child.cost > std::numeric_limits<size_t>::max() - total) {
      return std::numeric_limits<size_t>::max();
    }
    total += child.cost;
  }
  return total;
}

template <typename L, typename A>
inline PathEntry shortestExplanationPath(const EGraph<L, A> &egraph, Id lhs,
                                         Id rhs, PathMemo &memo,
                                         ActivePairs &active) {
  auto key = std::make_pair(lhs, rhs);
  if (auto it = memo.find(key); it != memo.end()) {
    return it->second;
  }

  if (lhs == rhs) {
    PathEntry entry{0, {}};
    memo.emplace(key, entry);
    return entry;
  }

  if (!active.insert(key).second) {
    auto fallback_path = getParentPath(egraph, lhs, rhs);
    PathEntry fallback;
    fallback.path =
        fallback_path.value_or(std::vector<ExplanationConnection>{});
    fallback.cost = fallback.path.size();
    return fallback;
  }

  PathEntry best;
  auto fallback_path = getParentPath(egraph, lhs, rhs);
  if (fallback_path) {
    best.path = *fallback_path;
    best.cost = best.path.size();
  }

  struct QueueEntry {
    size_t cost;
    Id id;
    bool operator<(const QueueEntry &other) const { return cost > other.cost; }
  };

  std::priority_queue<QueueEntry> pq;
  std::unordered_map<Id, size_t> dist;
  std::unordered_map<Id, std::pair<Id, ExplanationConnection>> prev;
  pq.push({0, lhs});
  dist.emplace(lhs, 0);

  const auto &nodes = egraph.explanationNodes();
  while (!pq.empty()) {
    auto [cost_so_far, current] = pq.top();
    pq.pop();
    if (dist.at(current) != cost_so_far) {
      continue;
    }
    if (current == rhs) {
      break;
    }
    if (current.index() >= nodes.size()) {
      continue;
    }

    for (const auto &edge : nodes[current.index()].neighbors) {
      size_t edge_cost = connectionCost(egraph, edge, memo, active);
      if (edge_cost >= std::numeric_limits<size_t>::max() / 8) {
        continue;
      }
      size_t next_cost = cost_so_far + edge_cost;
      auto it = dist.find(edge.next);
      if (it != dist.end() && it->second <= next_cost) {
        continue;
      }
      dist[edge.next] = next_cost;
      prev[edge.next] = {current, edge};
      pq.push({next_cost, edge.next});
    }
  }

  if (auto it = dist.find(rhs); it != dist.end()) {
    std::vector<ExplanationConnection> path;
    for (Id cursor = rhs; cursor != lhs; cursor = prev.at(cursor).first) {
      path.push_back(prev.at(cursor).second);
    }
    std::reverse(path.begin(), path.end());
    if (it->second <= best.cost) {
      best.cost = it->second;
      best.path = std::move(path);
    }
  }

  active.erase(key);
  memo.emplace(key, best);
  return best;
}

template <typename L, typename A>
inline std::shared_ptr<TreeTerm<L>>
nodeToExplanation(const EGraph<L, A> &egraph, Id id) {
  auto term = std::make_shared<TreeTerm<L>>();
  term->expr = egraph.originalExpr(id);
  term->current = id;
  term->last = id;
  return term;
}

template <typename L, typename A>
inline std::shared_ptr<TreeTerm<L>> explainAdjacent(
    const EGraph<L, A> &egraph, const ExplanationConnection &connection,
    std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>,
                       PairHash> &cache);

template <typename L, typename A>
inline typename Explanation<L>::TreeExplanation
explainEnodes(const EGraph<L, A> &egraph, Id left, Id right,
              std::unordered_map<std::pair<Id, Id>,
                                 std::shared_ptr<TreeTerm<L>>, PairHash> &cache,
              PathMemo *path_memo = nullptr, ActivePairs *active = nullptr) {
  using TreeExplanation = typename Explanation<L>::TreeExplanation;
  TreeExplanation proof;
  proof.push_back(nodeToExplanation(egraph, left));
  if (left == right) {
    return proof;
  }

  std::vector<ExplanationConnection> path;
  if (egraph.optimizeExplanationLengths()) {
    if (!path_memo || !active) {
      PathMemo local_memo;
      ActivePairs local_active;
      path =
          shortestExplanationPath(egraph, left, right, local_memo, local_active)
              .path;
    } else {
      path = shortestExplanationPath(egraph, left, right, *path_memo, *active)
                 .path;
    }
  } else {
    auto parent_path = getParentPath(egraph, left, right);
    if (parent_path) {
      path = std::move(*parent_path);
    }
  }
  if (path.empty() && left != right) {
    return proof;
  }

  for (const auto &connection : path) {
    proof.push_back(
        explainAdjacent(egraph, connection, cache, path_memo, active));
  }
  return proof;
}

template <typename L, typename A>
inline std::shared_ptr<TreeTerm<L>> explainAdjacent(
    const EGraph<L, A> &egraph, const ExplanationConnection &connection,
    std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>,
                       PairHash> &cache,
    PathMemo *path_memo = nullptr, ActivePairs *active = nullptr) {
  auto key = std::make_pair(connection.current, connection.next);
  if (auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }

  std::shared_ptr<TreeTerm<L>> tree;
  if (connection.justification == ExplanationJustificationKind::Rule) {
    tree = nodeToExplanation(egraph, connection.next);
    if (connection.is_rewrite_forward) {
      tree->forward_rule = connection.rule;
    } else {
      tree->backward_rule = connection.rule;
    }
    tree->current = connection.next;
    tree->last = connection.current;
  } else {
    tree = nodeToExplanation(egraph, connection.current);
    const auto &left_node = egraph.originalNode(connection.current);
    const auto &right_node = egraph.originalNode(connection.next);
    if (left_node.matches(right_node)) {
      auto left_children = left_node.children();
      auto right_children = right_node.children();
      for (size_t i = 0; i < left_children.size() && i < right_children.size();
           ++i) {
        tree->child_proofs.push_back(explainEnodes(egraph, left_children[i],
                                                   right_children[i], cache,
                                                   path_memo, active));
      }
    }
    tree->current = connection.current;
    tree->last = connection.next;
  }

  cache.emplace(key, tree);
  return tree;
}

} // namespace detail

template <typename L, typename A>
inline std::optional<Explanation<L>>
explainEquivalence(const EGraph<L, A> &egraph, const RecExpr<L> &lhs,
                   const RecExpr<L> &rhs) {
  EGraph<L, A> clone = egraph;
  Id left = clone.addExprUncanonical(lhs);
  Id right = clone.addExprUncanonical(rhs);
  clone.rebuild();
  if (clone.find(left) != clone.find(right)) {
    return std::nullopt;
  }
  return explainEquivalence(clone, left, right);
}

template <typename L, typename A>
inline std::optional<Explanation<L>>
explainMatches(const EGraph<L, A> &egraph, const RecExpr<L> &left,
               const PatternAst<L> &right, const Subst &subst) {
  EGraph<L, A> clone = egraph;
  Id left_id = clone.addExprUncanonical(left);
  Id right_id = clone.addInstantiationUncanonical(right, subst);
  clone.rebuild();
  if (clone.find(left_id) != clone.find(right_id)) {
    return std::nullopt;
  }
  return explainEquivalence(clone, left_id, right_id);
}

template <typename L, typename A>
inline std::optional<Explanation<L>>
explainEquivalence(const EGraph<L, A> &egraph, Id lhs, Id rhs) {
  if (egraph.find(lhs) != egraph.find(rhs)) {
    return std::nullopt;
  }

  std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>, PairHash>
      cache;
  detail::PathMemo path_memo;
  detail::ActivePairs active;
  return Explanation<L>(
      detail::explainEnodes(egraph, lhs, rhs, cache, &path_memo, &active));
}

template <typename L, typename A>
inline bool checkEachExplain(const EGraph<L, A> &egraph,
                             const std::vector<Rewrite<L, A>> &rules) {
  std::unordered_map<std::string, const Rewrite<L, A> *> by_name;
  for (const auto &rule : rules) {
    by_name.emplace(rule.name(), &rule);
  }

  const auto &nodes = egraph.explanationNodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto &connection = nodes[i].parent_connection;
    if (connection.next == Id::fromIndex(i) ||
        connection.justification != ExplanationJustificationKind::Rule) {
      continue;
    }

    auto it = by_name.find(connection.rule);
    if (it == by_name.end()) {
      continue;
    }
    const auto *searcher_ast = it->second->searcher().getPatternAst();
    const auto *applier_ast = it->second->applier().getPatternAst();
    if (!searcher_ast || !applier_ast) {
      continue;
    }

    EGraph<L, A> validation(egraph.analysis());
    Id current_id = validation.addExpr(egraph.originalExpr(connection.current));
    Id next_id = validation.addExpr(egraph.originalExpr(connection.next));
    validation.rebuild();

    const PatternAst<L> *lhs_ast =
        connection.is_rewrite_forward ? searcher_ast : applier_ast;
    const PatternAst<L> *rhs_ast =
        connection.is_rewrite_forward ? applier_ast : searcher_ast;
    Pattern<L> lhs_pattern(*lhs_ast);

    auto matches = lhs_pattern.searchEClassWithLimit(validation, current_id, 8);
    if (!matches || matches->substs.empty()) {
      return false;
    }

    bool any_valid = false;
    for (const auto &subst : matches->substs) {
      Id applied = validation.addInstantiation(*rhs_ast, subst);
      validation.rebuild();
      if (validation.find(applied) == validation.find(next_id)) {
        any_valid = true;
        break;
      }
    }
    if (!any_valid) {
      return false;
    }
  }
  return true;
}

} // namespace lotus::egraph
