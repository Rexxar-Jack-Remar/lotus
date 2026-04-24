#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/Pattern.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/Subst.h"

#include <memory>
#include <queue>

namespace lotus::egraph {

template <typename L, typename A> class Rewrite;

template <typename L> struct FlatTerm {
  RecExpr<L> expr;
  std::optional<Symbol> backward_rule;
  std::optional<Symbol> forward_rule;
  std::vector<FlatTerm<L>> children;

  static FlatTerm fromExpr(const RecExpr<L> &expr, Id id) {
    const auto &node = expr[id];
    std::vector<FlatTerm<L>> children;
    children.reserve(node.children().size());
    for (Id child : node.children()) {
      children.push_back(fromExpr(expr, child));
    }
    return fromNodeAndChildren(node, std::move(children));
  }

  std::string toString() const { return formatDisplay(*this); }

  bool hasRewriteForward() const {
    if (forward_rule) {
      return true;
    }
    for (const auto &child : children) {
      if (child.hasRewriteForward()) {
        return true;
      }
    }
    return false;
  }

  bool hasRewriteBackward() const {
    if (backward_rule) {
      return true;
    }
    for (const auto &child : children) {
      if (child.hasRewriteBackward()) {
        return true;
      }
    }
    return false;
  }

  FlatTerm removeRewrites() const {
    FlatTerm copy = *this;
    copy.backward_rule.reset();
    copy.forward_rule.reset();
    for (auto &child : copy.children) {
      child = child.removeRewrites();
    }
    return copy;
  }

  void combineRewrites(const FlatTerm &other) {
    if (other.forward_rule) {
      if (forward_rule) {
        throw std::runtime_error(
            "Invalid explanation: duplicate forward rewrite annotation");
      }
      forward_rule = other.forward_rule;
    }
    if (other.backward_rule) {
      if (backward_rule) {
        throw std::runtime_error(
            "Invalid explanation: duplicate backward rewrite annotation");
      }
      backward_rule = other.backward_rule;
    }
    for (size_t i = 0; i < children.size() && i < other.children.size(); ++i) {
      children[i].combineRewrites(other.children[i]);
    }
  }

  static FlatTerm fromNodeAndChildren(const L &node,
                                      std::vector<FlatTerm<L>> children) {
    FlatTerm term;
    term.children = std::move(children);

    RecExpr<L> expr;
    std::vector<Id> child_roots;
    child_roots.reserve(term.children.size());
    for (const auto &child : term.children) {
      child_roots.push_back(appendExpr(expr, child.expr));
    }
    auto rebuilt = node;
    size_t child_index = 0;
    for (Id &child : rebuilt.childrenMut()) {
      child = child_roots.at(child_index++);
    }
    expr.add(rebuilt);
    term.expr = std::move(expr);
    return term;
  }

private:
  static std::string formatDisplay(const FlatTerm &term) {
    const auto &root = term.expr[term.expr.root()];
    std::string out;
    if (term.children.empty()) {
      out = displayNode(root);
    } else {
      std::vector<std::string> parts;
      parts.reserve(term.children.size() + 1);
      parts.push_back(displayNode(root));
      for (const auto &child : term.children) {
        parts.push_back(formatDisplay(child));
      }
      out = "(" + joinStrings(parts, " ") + ")";
    }

    if (term.forward_rule) {
      out = "(Rewrite=> " + std::string(term.forward_rule->view()) + " " +
            out + ")";
    } else if (term.backward_rule) {
      out = "(Rewrite<= " + std::string(term.backward_rule->view()) + " " +
            out + ")";
    }
    return out;
  }

  static Id appendExpr(RecExpr<L> &dst, const RecExpr<L> &src) {
    std::vector<Id> ids;
    ids.reserve(src.size());
    for (const auto &node : src.items()) {
      ids.push_back(
          dst.add(node.mapChildren([&](Id id) { return ids[id.index()]; })));
    }
    return ids.back();
  }
};

template <typename L> struct TreeTerm {
  RecExpr<L> expr;
  std::optional<Symbol> backward_rule;
  std::optional<Symbol> forward_rule;
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

  template <typename R, typename A>
  void checkProof(R &&rules) const {
    using RewriteT = Rewrite<L, A>;
    std::vector<const RewriteT *> collected;
    for (const auto &rule : rules) {
      collected.push_back(&rule);
    }

    std::unordered_map<Symbol, const RewriteT *> table;
    for (const RewriteT *rule : collected) {
      table.emplace(rule->name(), rule);
    }

    const auto &flat = makeFlatExplanation();
    if (flat.empty()) {
      return;
    }

    if (flat.front().hasRewriteForward() || flat.front().hasRewriteBackward()) {
      throw std::runtime_error(
          "Invalid explanation: first flat step carries a rewrite annotation");
    }

    for (size_t i = 0; i + 1 < flat.size(); ++i) {
      const auto &current = flat[i];
      const auto &next = flat[i + 1];
      bool has_forward = next.hasRewriteForward();
      bool has_backward = next.hasRewriteBackward();
      if (has_forward == has_backward) {
        throw std::runtime_error(
            "Invalid explanation: each step must contain exactly one rewrite");
      }

      bool ok = has_forward ? checkRewriteAt(current, next, table, true)
                            : checkRewriteAt(current, next, table, false);
      if (!ok) {
        throw std::runtime_error("Explanation proof check failed");
      }
    }
  }

  std::string getString() const {
    if (explanation_trees_.empty()) {
      return "(Explanation)";
    }
    std::vector<std::string> parts;
    for (const auto &tree : explanation_trees_) {
      parts.push_back(formatTree(*tree));
    }
    return "(Explanation " + joinStrings(parts, " ") + ")";
  }
  std::string getStringWithLet() const {
    std::unordered_set<const TreeTerm<L> *> shared;
    std::vector<const TreeTerm<L> *> to_bind;
    for (const auto &tree : explanation_trees_) {
      findSharedTerms(tree, shared, to_bind);
    }

    std::unordered_map<const TreeTerm<L> *, std::string> bindings;
    std::vector<std::pair<std::string, const TreeTerm<L> *>> ordered_bindings;
    for (const TreeTerm<L> *tree : to_bind) {
      if (bindings.count(tree)) {
        continue;
      }
      std::string name = "v_" + std::to_string(bindings.size());
      bindings.emplace(tree, name);
      ordered_bindings.emplace_back(name, tree);
    }

    std::vector<std::string> parts;
    for (const auto &tree : explanation_trees_) {
      parts.push_back(formatTreeWithBindings(tree, bindings, true));
    }
    std::string result = parts.empty()
                             ? "(Explanation)"
                             : "(Explanation " + joinStrings(parts, " ") + ")";
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
      out = "(Rewrite=> " + std::string(tree.forward_rule->view()) + " " +
            out + ")";
    } else if (tree.backward_rule) {
      out = "(Rewrite<= " + std::string(tree.backward_rule->view()) + " " +
            out + ")";
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
    return term.toString();
  }

  static std::string formatTreeWithBindings(
      const std::shared_ptr<TreeTerm<L>> &tree,
      const std::unordered_map<const TreeTerm<L> *, std::string> &bindings,
      bool is_root = false) {
    if (!is_root) {
      if (auto it = bindings.find(tree.get()); it != bindings.end()) {
        return it->second;
      }
    }

    std::string out = tree->expr.toString();
    if (tree->forward_rule) {
      out = "(Rewrite=> " + std::string(tree->forward_rule->view()) + " " +
            out + ")";
    } else if (tree->backward_rule) {
      out = "(Rewrite<= " + std::string(tree->backward_rule->view()) + " " +
            out + ")";
    }
    if (!tree->child_proofs.empty()) {
      std::vector<std::string> children;
      for (const auto &proofs : tree->child_proofs) {
        std::vector<std::string> nested;
        for (const auto &child : proofs) {
          nested.push_back(formatTreeWithBindings(child, bindings));
        }
        children.push_back("(Explanation " + joinStrings(nested, "\n") + ")");
      }
      out = "(Explanation " + out + " " + joinStrings(children, " ") + ")";
    }
    return out;
  }

  static std::string formatTreeWithBindings(
      const TreeTerm<L> &tree,
      const std::unordered_map<const TreeTerm<L> *, std::string> &bindings,
      bool is_root = false) {
    if (!is_root) {
      if (auto it = bindings.find(&tree); it != bindings.end()) {
        return it->second;
      }
    }

    std::string out = tree.expr.toString();
    if (tree.forward_rule) {
      out = "(Rewrite=> " + std::string(tree.forward_rule->view()) + " " +
            out + ")";
    } else if (tree.backward_rule) {
      out = "(Rewrite<= " + std::string(tree.backward_rule->view()) + " " +
            out + ")";
    }
    if (!tree.child_proofs.empty()) {
      std::vector<std::string> children;
      for (const auto &proofs : tree.child_proofs) {
        std::vector<std::string> nested;
        for (const auto &child : proofs) {
          nested.push_back(formatTreeWithBindings(child, bindings));
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
      auto explanation = flattenTree(*tree);
      if (!flat.empty() && !explanation.empty() &&
          !explanation.front().hasRewriteForward() &&
          !explanation.front().hasRewriteBackward()) {
        FlatTerm<L> last = flat.back();
        flat.pop_back();
        explanation.front().combineRewrites(last);
      }
      flat.insert(flat.end(), explanation.begin(), explanation.end());
    }
    return flat;
  }

  static FlatExplanation flattenTree(const TreeTerm<L> &tree) {
    const auto &root = tree.expr[tree.expr.root()];

    std::vector<FlatExplanation> child_proofs;
    std::vector<FlatTerm<L>> representative_terms;
    child_proofs.reserve(tree.child_proofs.size());
    representative_terms.reserve(tree.child_proofs.size());

    for (const auto &child_explanation : tree.child_proofs) {
      auto flat_proof = flattenProof(child_explanation);
      representative_terms.push_back(flat_proof.front().removeRewrites());
      child_proofs.push_back(std::move(flat_proof));
    }

    FlatExplanation proof;
    proof.push_back(FlatTerm<L>::fromNodeAndChildren(root, representative_terms));

    for (size_t i = 0; i < child_proofs.size(); ++i) {
      proof.back().children[i] = child_proofs[i].front();

      for (size_t j = 1; j < child_proofs[i].size(); ++j) {
        std::vector<FlatTerm<L>> children;
        children.reserve(proof.back().children.size());
        for (size_t k = 0; k < proof.back().children.size(); ++k) {
          if (k == i) {
            children.push_back(child_proofs[i][j]);
          } else {
            children.push_back(representative_terms[k]);
          }
        }
        proof.push_back(FlatTerm<L>::fromNodeAndChildren(root, std::move(children)));
      }

      representative_terms[i] = child_proofs[i].back().removeRewrites();
    }

    proof.front().backward_rule = tree.backward_rule;
    proof.front().forward_rule = tree.forward_rule;
    return proof;
  }

  template <typename A>
  static std::unordered_map<Symbol, const Rewrite<L, A> *>
  makeRuleTable(const std::vector<const Rewrite<L, A> *> &rules) {
    std::unordered_map<Symbol, const Rewrite<L, A> *> table;
    for (const Rewrite<L, A> *rule : rules) {
      table.emplace(rule->name(), rule);
    }
    return table;
  }

  template <typename A>
  static bool checkRewriteAt(
      const FlatTerm<L> &current, const FlatTerm<L> &next,
      const std::unordered_map<Symbol, const Rewrite<L, A> *> &table,
      bool is_forward) {
    if (is_forward && next.forward_rule) {
      auto it = table.find(*next.forward_rule);
      return it == table.end() ? true : checkRewrite(current, next, *it->second);
    }
    if (!is_forward && next.backward_rule) {
      auto it = table.find(*next.backward_rule);
      return it == table.end() ? true : checkRewrite(next, current, *it->second);
    }

    if (current.children.size() != next.children.size()) {
      return false;
    }

    for (size_t i = 0; i < current.children.size() && i < next.children.size();
         ++i) {
      if (!checkRewriteAt(current.children[i], next.children[i], table,
                          is_forward)) {
        return false;
      }
    }
    return true;
  }

  template <typename A>
  static bool checkRewrite(const FlatTerm<L> &current, const FlatTerm<L> &next,
                           const Rewrite<L, A> &rewrite) {
    const PatternAst<L> *lhs = rewrite.searcher().getPatternAst();
    const PatternAst<L> *rhs = rewrite.applier().getPatternAst();
    if (!lhs || !rhs) {
      return true;
    }

    FlatTerm<L> rewritten = rewriteFlatTerm(current, *lhs, *rhs);
    return flattenedEqual(rewritten, next);
  }

  static FlatTerm<L> rewriteFlatTerm(const FlatTerm<L> &term,
                                     const PatternAst<L> &lhs,
                                     const PatternAst<L> &rhs) {
    std::unordered_map<Var, const FlatTerm<L> *> bindings;
    makeBindings(term, lhs, lhs.root(), bindings);
    return fromPattern(rhs, rhs.root(), bindings);
  }

  static FlatTerm<L>
  fromPattern(const PatternAst<L> &pattern, Id location,
              const std::unordered_map<Var, const FlatTerm<L> *> &bindings) {
    const auto &node = pattern[location];
    if (node.isVar()) {
      return *bindings.at(node.var());
    }

    std::vector<FlatTerm<L>> children;
    for (Id child : node.node().children()) {
      children.push_back(fromPattern(pattern, child, bindings));
    }
    return FlatTerm<L>::fromNodeAndChildren(node.node(), std::move(children));
  }

  static void makeBindings(const FlatTerm<L> &term, const PatternAst<L> &pattern,
                           Id location,
                           std::unordered_map<Var, const FlatTerm<L> *> &bindings) {
    const auto &item = pattern[location];
    if (item.isVar()) {
      auto it = bindings.find(item.var());
      if (it != bindings.end()) {
        if (!flattenedEqual(*it->second, term)) {
          throw std::runtime_error(
              "Invalid explanation: inconsistent variable binding in proof");
        }
      } else {
        bindings.emplace(item.var(), &term);
      }
      return;
    }

    const auto &node = item.node();
    if (!node.matches(term.expr[term.expr.root()])) {
      throw std::runtime_error("Invalid explanation: proof rewrite root mismatch");
    }
    for (size_t i = 0; i < node.children().size(); ++i) {
      makeBindings(term.children[i], pattern, node.children()[i], bindings);
    }
  }

  static bool flattenedEqual(const FlatTerm<L> &lhs, const FlatTerm<L> &rhs) {
    if (!lhs.expr[lhs.expr.root()].matches(rhs.expr[rhs.expr.root()])) {
      return false;
    }
    if (lhs.children.size() != rhs.children.size()) {
      return false;
    }
    for (size_t i = 0; i < lhs.children.size(); ++i) {
      if (!flattenedEqual(lhs.children[i], rhs.children[i])) {
        return false;
      }
    }
    return true;
  }

  static void findSharedTerms(const std::shared_ptr<TreeTerm<L>> &term,
                              std::unordered_set<const TreeTerm<L> *> &seen,
                              std::vector<const TreeTerm<L> *> &to_bind) {
    if (term->child_proofs.empty()) {
      return;
    }
    if (seen.insert(term.get()).second) {
      for (const auto &proofs : term->child_proofs) {
        for (const auto &child : proofs) {
          findSharedTerms(child, seen, to_bind);
        }
      }
    } else {
      to_bind.push_back(term.get());
    }
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
template <typename L>
using NodeExplanationCache =
    std::unordered_map<Id, std::shared_ptr<TreeTerm<L>>>;

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
  if (connection.justification.isRule()) {
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
nodeToExplanation(const EGraph<L, A> &egraph, Id id,
                  NodeExplanationCache<L> &node_cache) {
  if (auto it = node_cache.find(id); it != node_cache.end()) {
    return it->second;
  }
  auto term = std::make_shared<TreeTerm<L>>();
  term->expr = egraph.originalExpr(id);
  term->current = id;
  term->last = id;
  node_cache.emplace(id, term);
  return term;
}

template <typename L, typename A>
inline std::shared_ptr<TreeTerm<L>> explainAdjacent(
    const EGraph<L, A> &egraph, const ExplanationConnection &connection,
    std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>,
                       PairHash> &cache,
    NodeExplanationCache<L> &node_cache, PathMemo *path_memo = nullptr,
    ActivePairs *active = nullptr);

template <typename L, typename A>
inline typename Explanation<L>::TreeExplanation
explainEnodes(const EGraph<L, A> &egraph, Id left, Id right,
              std::unordered_map<std::pair<Id, Id>,
                                 std::shared_ptr<TreeTerm<L>>, PairHash> &cache,
              NodeExplanationCache<L> &node_cache,
              PathMemo *path_memo = nullptr, ActivePairs *active = nullptr) {
  using TreeExplanation = typename Explanation<L>::TreeExplanation;
  TreeExplanation proof;
  proof.push_back(nodeToExplanation(egraph, left, node_cache));
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
        explainAdjacent(egraph, connection, cache, node_cache, path_memo,
                        active));
  }
  return proof;
}

template <typename L, typename A>
inline std::shared_ptr<TreeTerm<L>> explainAdjacent(
    const EGraph<L, A> &egraph, const ExplanationConnection &connection,
    std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>,
                       PairHash> &cache,
    NodeExplanationCache<L> &node_cache, PathMemo *path_memo,
    ActivePairs *active) {
  auto key = std::make_pair(connection.current, connection.next);
  if (auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }

  std::shared_ptr<TreeTerm<L>> tree;
  if (connection.justification.isRule()) {
    tree = nodeToExplanation(egraph, connection.next, node_cache);
    if (connection.is_rewrite_forward) {
      tree->forward_rule = connection.justification.rule;
    } else {
      tree->backward_rule = connection.justification.rule;
    }
    tree->current = connection.next;
    tree->last = connection.current;
  } else {
    tree = nodeToExplanation(egraph, connection.current, node_cache);
    const auto &left_node = egraph.originalNode(connection.current);
    const auto &right_node = egraph.originalNode(connection.next);
    if (left_node.matches(right_node)) {
      auto left_children = left_node.children();
      auto right_children = right_node.children();
      for (size_t i = 0; i < left_children.size() && i < right_children.size();
           ++i) {
        tree->child_proofs.push_back(explainEnodes(egraph, left_children[i],
                                                   right_children[i], cache,
                                                   node_cache,
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
  if (!egraph.areExplanationsEnabled()) {
    return std::nullopt;
  }
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
  if (!egraph.areExplanationsEnabled()) {
    return std::nullopt;
  }
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
  if (!egraph.areExplanationsEnabled()) {
    return std::nullopt;
  }
  if (egraph.find(lhs) != egraph.find(rhs)) {
    return std::nullopt;
  }

  std::unordered_map<std::pair<Id, Id>, std::shared_ptr<TreeTerm<L>>, PairHash>
      cache;
  detail::NodeExplanationCache<L> node_cache;
  detail::PathMemo path_memo;
  detail::ActivePairs active;
  return Explanation<L>(
      detail::explainEnodes(egraph, lhs, rhs, cache, node_cache, &path_memo,
                            &active));
}

template <typename L, typename A>
inline bool checkEachExplain(const EGraph<L, A> &egraph,
                             const std::vector<Rewrite<L, A>> &rules) {
  std::unordered_map<Symbol, const Rewrite<L, A> *> by_name;
  for (const auto &rule : rules) {
    by_name.emplace(rule.name(), &rule);
  }

  const auto &nodes = egraph.explanationNodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (const auto &connection : nodes[i].neighbors) {
      if (!connection.justification.isRule()) {
        continue;
      }

      auto it = by_name.find(connection.justification.rule);
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
  }
  return true;
}

} // namespace lotus::egraph
