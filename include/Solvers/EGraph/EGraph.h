#pragma once

#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EClass.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/Subst.h"
#include "Solvers/EGraph/UnionFind.h"

namespace lotus::egraph {

template <typename L> struct ENodeOrVar;
template <typename L> class Pattern;
template <typename L> using PatternAst = RecExpr<ENodeOrVar<L>>;

struct UnionEvent {
  Id left;
  Id right;
  Justification justification = Justification::congruence();
  std::optional<std::string> left_expr;
  std::optional<std::string> right_expr;
};

struct ExplanationConnection {
  Justification justification = Justification::congruence();
  bool is_rewrite_forward = false;
  Id next = Id::fromIndex(0);
  Id current = Id::fromIndex(0);
};

struct ExplanationNode {
  std::vector<ExplanationConnection> neighbors;
  ExplanationConnection parent_connection;
};

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct LanguageMapper;

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct SimpleLanguageMapper;

template <typename L, typename AnalysisT> class EGraph {
public:
  using Analysis = AnalysisT;
  using Data = typename AnalysisT::Data;
  using Class = EClass<L, Data>;

  EGraph() = default;
  explicit EGraph(AnalysisT analysis) : analysis_(std::move(analysis)) {}

  AnalysisT &analysis() { return analysis_; }
  const AnalysisT &analysis() const { return analysis_; }

  std::vector<std::reference_wrapper<const Class>> classes() const {
    assertClean();
    std::vector<std::reference_wrapper<const Class>> out;
    out.reserve(classes_.size());
    for (const auto &[_, klass] : classes_) {
      out.emplace_back(klass);
    }
    std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.get().id < rhs.get().id;
    });
    return out;
  }

  std::vector<std::reference_wrapper<Class>> classesMut() {
    std::vector<std::reference_wrapper<Class>> out;
    out.reserve(classes_.size());
    for (auto &[_, klass] : classes_) {
      out.emplace_back(klass);
    }
    std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.get().id < rhs.get().id;
    });
    return out;
  }

  std::vector<Id> classesForOp(const typename L::Discriminant &op) const {
    assertClean();
    std::vector<Id> ids;
    auto it = classes_by_op_.find(op);
    if (it == classes_by_op_.end()) {
      return ids;
    }
    ids.insert(ids.end(), it->second.begin(), it->second.end());
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  const std::vector<L> &nodes() const { return nodes_; }
  const L &originalNode(Id id) const { return nodes_.at(id.index()); }

  bool empty() const { return memo_.empty(); }
  bool clean() const { return clean_; }
  size_t totalSize() const { return memo_.size(); }
  size_t totalNumberOfNodes() const {
    size_t total = 0;
    for (const auto &[_, klass] : classes_) {
      total += klass.nodes.size();
    }
    return total;
  }
  size_t numberOfClasses() const { return classes_.size(); }

  Id find(Id id) const { return union_find_.find(id); }
  Id findMut(Id id) { return union_find_.findMut(id); }

  Class &operator[](Id id) { return classes_.at(findMut(id)); }
  const Class &operator[](Id id) const { return classes_.at(find(id)); }

  std::vector<Id> classIds() const {
    assertClean();
    std::vector<Id> ids;
    ids.reserve(classes_.size());
    for (const auto &[id, _] : classes_) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  EGraph withExplanationsEnabled() const {
    if (explanations_enabled_) {
      return *this;
    }
    if (!nodes_.empty()) {
      throw std::runtime_error("Need to set explanations enabled before adding "
                               "any expressions to the egraph");
    }
    EGraph copy = *this;
    copy.explanations_enabled_ = true;
    copy.optimize_explanation_lengths_ = true;
    return copy;
  }

  EGraph withExplanationsDisabled() const {
    EGraph copy = *this;
    copy.explanations_enabled_ = false;
    copy.union_events_.clear();
    copy.explanation_nodes_.clear();
    copy.uncanonical_memo_.clear();
    return copy;
  }

  EGraph withoutExplanations() const { return withExplanationsDisabled(); }

  EGraph withoutExplanationLengthOptimization() const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Need to set explanations enabled before "
                               "setting length optimization");
    }
    EGraph copy = *this;
    copy.optimize_explanation_lengths_ = false;
    return copy;
  }

  EGraph withExplanationLengthOptimization() const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Need to set explanations enabled before "
                               "setting length optimization");
    }
    EGraph copy = *this;
    copy.optimize_explanation_lengths_ = true;
    return copy;
  }

  bool areExplanationsEnabled() const { return explanations_enabled_; }
  bool optimizeExplanationLengths() const {
    return optimize_explanation_lengths_;
  }
  const std::vector<UnionEvent> &unionEvents() const { return union_events_; }
  const std::vector<ExplanationNode> &explanationNodes() const {
    return explanation_nodes_;
  }

  std::vector<std::tuple<Id, Id, Symbol>> getUnionEqualities() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting union equalities");
    }
    std::vector<std::tuple<Id, Id, Symbol>> out;
    out.reserve(union_events_.size());
    for (const auto &event : union_events_) {
      if (event.justification.isRule()) {
        out.emplace_back(event.left, event.right, event.justification.rule);
      }
    }
    return out;
  }

  std::optional<Id> lookupInternal(L node) const {
    canonicalizeInPlace(node);
    return memoLookup(node);
  }

  EGraph copyWithoutUnions(AnalysisT analysis) const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Use withExplanationsEnabled before copying an "
                               "egraph without unions");
    }
    EGraph copy(std::move(analysis));
    for (const auto &node : nodes_) {
      copy.add(node);
    }
    return copy;
  }

  EGraph egraphIntersect(const EGraph &other, AnalysisT analysis) const {
    if (!clean_ || !other.clean_) {
      EGraph lhs = *this;
      EGraph rhs = other;
      if (!lhs.clean_) {
        lhs.rebuild();
      }
      if (!rhs.clean_) {
        rhs.rebuild();
      }
      return lhs.egraphIntersect(rhs, std::move(analysis));
    }

    std::unordered_map<std::pair<Id, Id>, Id, PairHash> product_map;
    std::vector<std::pair<L, Id>> enodes;

    for (const auto &class1 : classes()) {
      for (const auto &class2 : other.classes()) {
        intersectClasses(other, enodes, class1.get().id, class2.get().id,
                         product_map);
      }
    }

    return fromEnodes(std::move(enodes), std::move(analysis));
  }

  void egraphUnion(const EGraph &other) {
    if (!clean_) {
      rebuild();
    }
    if (!other.clean_) {
      EGraph copy = other;
      copy.rebuild();
      egraphUnion(copy);
      return;
    }

    for (const auto &[left, right, why] : other.getUnionEqualities()) {
      auto [left_pat, left_subst] = other.idToPattern(left, {});
      auto [right_pat, right_subst] = other.idToPattern(right, {});
      if (!left_subst.bindings().empty() || !right_subst.bindings().empty()) {
        throw std::runtime_error(
            "Unexpected variables while replaying egraph union");
      }
      unionInstantiations(left_pat.ast(), right_pat.ast(), Subst{}, why);
    }
    rebuild();
  }

  Id add(const L &node) { return find(addUncanonical(node)); }

  Id addUncanonical(const L &node) {
    clean_ = false;

    L canonical = node;
    canonicalizeInPlace(canonical);
    if (auto existing = memoLookup(canonical)) {
      if (!explanations_enabled_) {
        return *existing;
      }

      auto uncanon_it = uncanonical_memo_.find(node);
      if (uncanon_it != uncanonical_memo_.end()) {
        return uncanon_it->second;
      }

      Id new_id = union_find_.makeSet();
      ensureNodeStorage(new_id, node);
      uncanonical_memo_.emplace(node, new_id);
      union_find_.unite(find(*existing), new_id);
      recordExplanation(*existing, new_id, Justification::congruence());
      recordUnion(*existing, new_id, Justification::congruence());
      return new_id;
    }

    return makeNewEClass(canonical, node);
  }

  Id addExpr(const RecExpr<L> &expr) { return find(addExprUncanonical(expr)); }

  Id addExprUncanonical(const RecExpr<L> &expr) {
    std::vector<Id> ids;
    ids.reserve(expr.size());
    for (const auto &node : expr.items()) {
      ids.push_back(addUncanonical(
          node.mapChildren([&](Id id) { return ids[id.index()]; })));
    }
    return ids.back();
  }

  std::optional<Id> lookup(const L &node) const {
    assertClean();
    L canonical = node;
    canonicalizeInPlace(canonical);
    auto found = memoLookup(canonical);
    if (!found) {
      return std::nullopt;
    }
    return find(*found);
  }

  std::optional<Id> lookupExpr(const RecExpr<L> &expr) const {
    auto ids = lookupExprIds(expr);
    if (!ids || ids->empty()) {
      return std::nullopt;
    }
    return ids->back();
  }

  std::optional<std::vector<Id>> lookupExprIds(const RecExpr<L> &expr) const {
    assertClean();
    std::vector<Id> ids;
    ids.reserve(expr.size());
    for (const auto &node : expr.items()) {
      L mapped = node.mapChildren([&](Id id) { return ids[id.index()]; });
      auto found = lookup(mapped);
      if (!found) {
        return std::nullopt;
      }
      ids.push_back(*found);
    }
    return ids;
  }

  std::optional<std::vector<Id>>
  lookupExprUncanonicalIds(const RecExpr<L> &expr) const {
    assertClean();
    std::vector<Id> ids;
    ids.reserve(expr.size());

    for (const auto &node : expr.items()) {
      L materialized = node.mapChildren([&](Id id) { return ids[id.index()]; });

      std::optional<Id> exact;
      for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i] == materialized) {
          exact = Id::fromIndex(i);
          break;
        }
      }

      if (exact) {
        ids.push_back(*exact);
        continue;
      }

      auto canonical = lookup(materialized);
      if (!canonical) {
        return std::nullopt;
      }
      ids.push_back(*canonical);
    }

    return ids;
  }

  Id addInstantiation(const PatternAst<L> &pat, const Subst &subst);
  Id addInstantiationUncanonical(const PatternAst<L> &pat, const Subst &subst) {
    return addInstantiationNoncanonical(pat, subst);
  }

  Id unite(Id lhs, Id rhs, const Symbol &reason = {}) {
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    auto [id, _] = uniteImpl(lhs, rhs, justification);
    return id;
  }

  std::pair<Id, bool> uniteChecked(Id lhs, Id rhs,
                                   const Symbol &reason = {}) {
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    return uniteImpl(lhs, rhs, justification);
  }

  std::pair<Id, bool> unionInstantiations(const PatternAst<L> &from_pat,
                                          const PatternAst<L> &to_pat,
                                          const Subst &subst,
                                          const Symbol &reason);

  bool unionTrusted(Id from, Id to, const Symbol &reason) {
    return uniteImpl(from, to, Justification::ruleJustification(reason)).second;
  }

  std::vector<Id> equivs(const RecExpr<L> &expr1,
                         const RecExpr<L> &expr2) const {
    assertClean();
    Pattern<L> pat1(expr1);
    Pattern<L> pat2(expr2);
    auto matches1 = pat1.search(*this);
    auto matches2 = pat2.search(*this);

    std::vector<Id> out;
    for (const auto &lhs : matches1) {
      for (const auto &rhs : matches2) {
        if (find(lhs.eclass) == find(rhs.eclass)) {
          out.push_back(lhs.eclass);
        }
      }
    }
    return out;
  }

  bool equivalent(const RecExpr<L> &expr1, const RecExpr<L> &expr2) const {
    return !equivs(expr1, expr2).empty();
  }

  RecExpr<L> idToExpr(Id id) const {
    std::unordered_map<Id, Id> cache;
    RecExpr<L> expr;
    originalExprInternal(expr, id, cache);
    return expr;
  }

  RecExpr<L> originalExpr(Id id) const {
    std::unordered_map<Id, Id> cache;
    RecExpr<L> expr;
    originalExprInternal(expr, id, cache);
    return expr;
  }

  RecExpr<L> idToExprFlat(Id id) const {
    RecExpr<L> expr;
    const auto &node = classes_.at(find(id)).nodes.front();
    expr.add(node);
    return expr;
  }

  std::pair<Pattern<L>, Subst>
  idToPattern(Id id, const std::unordered_map<Id, Id> &substitutions) const;

  size_t getNumCongr() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting explanations");
    }
    std::unordered_set<std::pair<Id, Id>, PairHash> edges;
    for (size_t i = 0; i < explanation_nodes_.size(); ++i) {
      for (const auto &neighbor : explanation_nodes_[i].neighbors) {
        if (!neighbor.justification.isCongruence()) {
          continue;
        }
        Id a = Id::fromIndex(i);
        Id b = neighbor.next;
        if (b < a) {
          std::swap(a, b);
        }
        edges.emplace(a, b);
      }
    }
    return edges.size();
  }

  size_t getExplanationNumNodes() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting explanations");
    }
    return explanation_nodes_.size();
  }

  void setAnalysisData(Id id, Data data) {
    Id canonical = findMut(id);
    auto &klass = classes_.at(canonical);
    klass.data = std::move(data);
    for (Id parent : klass.parents) {
      analysis_pending_.push_back(parent);
    }
    analysis_.modify(*this, canonical);
  }

  size_t rebuild() {
    size_t unions = 0;
    unions += processPending();
    rebuildClasses();
    clean_ = true;
    return unions;
  }

private:
  template <typename SrcL, typename SrcA, typename DstL, typename DstA>
  friend struct LanguageMapper;

  void assertClean() const {
    if (!clean_) {
      throw std::runtime_error(
          "Tried to query a dirty e-graph; call rebuild() first");
    }
  }

  void ensureNodeStorage(Id id, const L &node) {
    if (nodes_.size() != id.index()) {
      throw std::runtime_error("EGraph node storage out of sync");
    }
    nodes_.push_back(node);
    if (explanations_enabled_) {
      if (explanation_nodes_.size() != id.index()) {
        throw std::runtime_error("EGraph explanation node storage out of sync");
      }
      explanation_nodes_.push_back(ExplanationNode{
          {},
          ExplanationConnection{
              Justification::congruence(), false, id, id}});
    }
  }

  void makeExplanationLeader(Id node) {
    auto next = explanation_nodes_.at(node.index()).parent_connection.next;
    if (next == node) {
      return;
    }

    makeExplanationLeader(next);
    const auto node_connection =
        explanation_nodes_.at(node.index()).parent_connection;
    explanation_nodes_.at(next.index()).parent_connection =
        ExplanationConnection{node_connection.justification,
                              !node_connection.is_rewrite_forward, node, next};
  }

  void addExplanationNeighbor(Id lhs, Id rhs,
                              const Justification &justification,
                              bool forward) {
    explanation_nodes_.at(lhs.index()).neighbors.push_back(
        ExplanationConnection{justification, forward, rhs, lhs});
  }

  void addAlternateRewrite(Id lhs, Id rhs, const Symbol &reason) {
    if (!explanations_enabled_ || lhs == rhs) {
      return;
    }
    Justification justification = Justification::ruleJustification(reason);
    explanation_nodes_.at(lhs.index()).neighbors.insert(
        explanation_nodes_.at(lhs.index()).neighbors.begin(),
        ExplanationConnection{justification, true, rhs, lhs});
    explanation_nodes_.at(rhs.index()).neighbors.insert(
        explanation_nodes_.at(rhs.index()).neighbors.begin(),
        ExplanationConnection{justification, false, lhs, rhs});
  }

  void recordExplanation(Id lhs, Id rhs,
                         const std::optional<Justification> &justification) {
    if (!explanations_enabled_) {
      return;
    }

    Justification edge_justification =
        justification.value_or(Justification::congruence());

    if (lhs == rhs) {
      if (edge_justification.isRule()) {
        addExplanationNeighbor(lhs, rhs, edge_justification, true);
        addExplanationNeighbor(rhs, lhs, edge_justification, false);
      }
      return;
    }

    makeExplanationLeader(lhs);
    explanation_nodes_.at(lhs.index()).parent_connection =
        ExplanationConnection{edge_justification, true, rhs, lhs};
    addExplanationNeighbor(lhs, rhs, edge_justification, true);
    addExplanationNeighbor(rhs, lhs, edge_justification, false);
  }

  void canonicalizeInPlace(L &node) const {
    for (Id &child : node.childrenMut()) {
      child = find(child);
    }
  }

  std::optional<Id> memoLookup(const L &node) const {
    auto it = memo_.find(node);
    if (it == memo_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  static Id getProductId(
      Id class1, Id class2,
      std::unordered_map<std::pair<Id, Id>, Id, PairHash> &product_map) {
    auto key = std::make_pair(class1, class2);
    if (auto it = product_map.find(key); it != product_map.end()) {
      return it->second;
    }
    Id id = Id::fromIndex(product_map.size());
    product_map.emplace(key, id);
    return id;
  }

  static EGraph fromEnodes(std::vector<std::pair<L, Id>> enodes,
                           AnalysisT analysis) {
    EGraph egraph(std::move(analysis));
    std::unordered_map<Id, Id> ids;

    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &[enode, id] : enodes) {
        bool valid = true;
        for (Id child : enode.children()) {
          if (!ids.count(child)) {
            valid = false;
            break;
          }
        }
        if (!valid) {
          continue;
        }

        L mapped = enode.mapChildren([&](Id child) { return ids.at(child); });
        if (egraph.lookupInternal(mapped)) {
          continue;
        }

        Id added = egraph.add(mapped);
        auto [it, inserted] = ids.try_emplace(id, added);
        if (!inserted) {
          egraph.unite(it->second, added);
        }
        changed = true;
      }
    }

    return egraph;
  }

  void intersectClasses(
      const EGraph &other, std::vector<std::pair<L, Id>> &result, Id class1,
      Id class2,
      std::unordered_map<std::pair<Id, Id>, Id, PairHash> &product_map) const {
    Id result_id = getProductId(class1, class2, product_map);
    for (const auto &node1 : classes_.at(class1).nodes) {
      for (const auto &node2 : other.classes_.at(class2).nodes) {
        if (!node1.matches(node2)) {
          continue;
        }

        auto merged = node1;
        for (size_t i = 0; i < node1.children().size(); ++i) {
          merged.childrenMut()[i] =
              getProductId(find(node1.children()[i]),
                           other.find(node2.children()[i]), product_map);
        }
        result.emplace_back(std::move(merged), result_id);
      }
    }
  }

  Id makeNewEClass(const L &canonical, const L &original) {
    Id id = union_find_.makeSet();
    ensureNodeStorage(id, original);

    Class klass{id, {canonical}, AnalysisT::make(*this, original, id), {}};
    for (Id child : canonical.children()) {
      classes_.at(findMut(child)).parents.push_back(id);
    }

    classes_.emplace(id, std::move(klass));
    memo_[canonical] = id;
    pending_.push_back(id);
    analysis_.modify(*this, id);
    return id;
  }

  Id addInstantiationNoncanonical(const PatternAst<L> &pat, const Subst &subst);

  std::pair<Id, bool> uniteImpl(Id lhs, Id rhs,
                                const std::optional<Justification> &justification) {
    analysis_.preUnion(*this, lhs, rhs, justification);

    clean_ = false;
    Id original_lhs = lhs;
    Id original_rhs = rhs;
    Id lhs_class = findMut(lhs);
    Id rhs_class = findMut(rhs);
    if (lhs_class == rhs_class) {
      if (justification && justification->isRule() && explanations_enabled_) {
        addAlternateRewrite(original_lhs, original_rhs, justification->rule);
        recordUnion(original_lhs, original_rhs, justification);
      }
      return {lhs_class, false};
    }

    if (classes_.at(lhs_class).parents.size() < classes_.at(rhs_class).parents.size()) {
      std::swap(lhs_class, rhs_class);
    }

    recordExplanation(original_lhs, original_rhs, justification);
    recordUnion(original_lhs, original_rhs, justification);
    union_find_.unite(lhs_class, rhs_class);

    auto right_class = std::move(classes_.at(rhs_class));
    classes_.erase(rhs_class);
    auto &left_class = classes_.at(lhs_class);

    pending_.insert(pending_.end(), right_class.parents.begin(),
                    right_class.parents.end());
    DidMerge did_merge =
        analysis_.merge(left_class.data, std::move(right_class.data));
    if (did_merge.left_changed) {
      analysis_pending_.insert(analysis_pending_.end(),
                               left_class.parents.begin(),
                               left_class.parents.end());
    }
    if (did_merge.right_changed) {
      analysis_pending_.insert(analysis_pending_.end(),
                               right_class.parents.begin(),
                               right_class.parents.end());
    }

    left_class.nodes.insert(left_class.nodes.end(), right_class.nodes.begin(),
                            right_class.nodes.end());
    left_class.parents.insert(left_class.parents.end(),
                              right_class.parents.begin(),
                              right_class.parents.end());

    analysis_.modify(*this, lhs_class);
    return {lhs_class, true};
  }

  void recordUnion(Id lhs, Id rhs,
                   const std::optional<Justification> &justification) {
    if (explanations_enabled_) {
      union_events_.push_back(
          {lhs, rhs, justification.value_or(Justification::congruence()),
           tryIdToString(lhs), tryIdToString(rhs)});
    }
  }

  std::optional<std::string> tryIdToString(Id id) const {
    auto it = classes_.find(find(id));
    if (it == classes_.end() || it->second.nodes.empty()) {
      return std::nullopt;
    }

    try {
      return idToExpr(id).toString();
    } catch (...) {
      return std::nullopt;
    }
  }

  void rebuildClasses() {
    classes_by_op_.clear();

    for (auto &[_, klass] : classes_) {
      for (auto &node : klass.nodes) {
        canonicalizeInPlace(node);
      }
      std::sort(klass.nodes.begin(), klass.nodes.end());
      klass.nodes.erase(std::unique(klass.nodes.begin(), klass.nodes.end()),
                        klass.nodes.end());

      for (size_t i = 0; i < klass.nodes.size(); ++i) {
        if (i == 0 || !klass.nodes[i - 1].matches(klass.nodes[i])) {
          classes_by_op_[klass.nodes[i].discriminant()].insert(klass.id);
        }
      }
    }
  }

  size_t processPending() {
    size_t unions = 0;
    while (!pending_.empty() || !analysis_pending_.empty()) {
      while (!pending_.empty()) {
        Id id = pending_.back();
        pending_.pop_back();

        L node = nodes_.at(id.index());
        canonicalizeInPlace(node);

        auto existing = memo_.find(node);
        if (existing != memo_.end()) {
          auto [_, changed] =
              uniteImpl(existing->second, id, Justification::congruence());
          unions += changed ? 1u : 0u;
        } else {
          memo_[node] = id;
        }
      }

      while (!analysis_pending_.empty()) {
        Id class_id = analysis_pending_.back();
        analysis_pending_.pop_back();

        if (class_id.index() >= nodes_.size()) {
          continue;
        }

        L node = nodes_[class_id.index()];
        Id canonical = findMut(class_id);
        auto it = classes_.find(canonical);
        if (it == classes_.end()) {
          continue;
        }

        Data node_data = AnalysisT::remake(*this, node, canonical);
        DidMerge did_merge =
            analysis_.merge(it->second.data, std::move(node_data));
        if (did_merge.left_changed) {
          analysis_pending_.insert(analysis_pending_.end(),
                                   it->second.parents.begin(),
                                   it->second.parents.end());
          analysis_.modify(*this, canonical);
        }
      }
    }

    recomputeParents();
    return unions;
  }

  void recomputeParents() {
    for (auto &[_, klass] : classes_) {
      klass.parents.clear();
    }
    for (auto &[id, klass] : classes_) {
      for (const auto &node : klass.nodes) {
        for (Id child : node.children()) {
          classes_.at(findMut(child)).parents.push_back(id);
        }
      }
    }
  }

  Id idToExprInternal(RecExpr<L> &expr, Id id,
                      std::unordered_map<Id, Id> &cache) const {
    auto it = cache.find(id);
    if (it != cache.end()) {
      return it->second;
    }

    const auto &node = classes_.at(find(id)).nodes.front();
    auto materialized = node.mapChildren(
        [&](Id child) { return idToExprInternal(expr, find(child), cache); });
    Id added = expr.add(materialized);
    cache.emplace(id, added);
    return added;
  }

  Id originalExprInternal(RecExpr<L> &expr, Id id,
                          std::unordered_map<Id, Id> &cache) const {
    if (auto it = cache.find(id); it != cache.end()) {
      return it->second;
    }

    const auto &node = nodes_.at(id.index());
    auto materialized = node.mapChildren(
        [&](Id child) { return originalExprInternal(expr, child, cache); });
    Id added = expr.add(materialized);
    cache.emplace(id, added);
    return added;
  }

  Id idToPatternInternal(RecExpr<ENodeOrVar<L>> &pat, Id id,
                         const std::unordered_map<Id, Id> &substitutions,
                         Subst &subst, std::unordered_map<Id, Id> &cache) const;

  AnalysisT analysis_;
  UnionFind union_find_;
  std::vector<L> nodes_;
  std::unordered_map<L, Id> memo_;
  std::unordered_map<L, Id> uncanonical_memo_;
  std::vector<Id> pending_;
  std::vector<Id> analysis_pending_;
  std::unordered_map<Id, Class> classes_;
  std::unordered_map<typename L::Discriminant, std::unordered_set<Id>>
      classes_by_op_;
  std::vector<UnionEvent> union_events_;
  std::vector<ExplanationNode> explanation_nodes_;
  bool clean_ = false;
  bool explanations_enabled_ = false;
  bool optimize_explanation_lengths_ = true;
};

} // namespace lotus::egraph

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct LanguageMapper {
  using SourceGraph = EGraph<SrcL, SrcA>;
  using TargetGraph = EGraph<DstL, DstA>;
  using TargetData = typename DstA::Data;

  virtual ~LanguageMapper() = default;

  virtual DstL mapNode(const SrcL &node) const = 0;
  virtual typename DstL::Discriminant
  mapDiscriminant(const typename SrcL::Discriminant &discriminant) const = 0;
  virtual DstA mapAnalysis(const SrcA &analysis) const = 0;
  virtual TargetData mapData(const typename SrcA::Data &data) const = 0;

  EClass<DstL, TargetData>
  mapEClass(const EClass<SrcL, typename SrcA::Data> &src_eclass) const {
    EClass<DstL, TargetData> result;
    result.id = src_eclass.id;
    result.nodes.reserve(src_eclass.nodes.size());
    for (const auto &node : src_eclass.nodes) {
      result.nodes.push_back(mapNode(node));
    }
    result.data = mapData(src_eclass.data);
    result.parents = src_eclass.parents;
    return result;
  }

  TargetGraph mapEGraph(const SourceGraph &src_egraph) const {
    TargetGraph dst_egraph(mapAnalysis(src_egraph.analysis_));
    dst_egraph.union_find_ = src_egraph.union_find_;
    dst_egraph.nodes_.reserve(src_egraph.nodes_.size());
    for (const auto &node : src_egraph.nodes_) {
      dst_egraph.nodes_.push_back(mapNode(node));
    }

    for (const auto &[node, id] : src_egraph.memo_) {
      dst_egraph.memo_.emplace(mapNode(node), id);
    }
    for (const auto &[node, id] : src_egraph.uncanonical_memo_) {
      dst_egraph.uncanonical_memo_.emplace(mapNode(node), id);
    }

    dst_egraph.pending_ = src_egraph.pending_;
    dst_egraph.analysis_pending_ = src_egraph.analysis_pending_;
    dst_egraph.clean_ = src_egraph.clean_;
    dst_egraph.explanations_enabled_ = src_egraph.explanations_enabled_;
    dst_egraph.optimize_explanation_lengths_ =
        src_egraph.optimize_explanation_lengths_;

    for (const auto &[id, klass] : src_egraph.classes_) {
      dst_egraph.classes_.emplace(id, mapEClass(klass));
    }
    for (const auto &[discriminant, ids] : src_egraph.classes_by_op_) {
      dst_egraph.classes_by_op_.emplace(mapDiscriminant(discriminant), ids);
    }
    for (const auto &event : src_egraph.union_events_) {
      dst_egraph.union_events_.push_back({event.left, event.right,
                                          event.justification, std::nullopt,
                                          std::nullopt});
    }
    dst_egraph.explanation_nodes_ = src_egraph.explanation_nodes_;

    return dst_egraph;
  }
};

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct SimpleLanguageMapper final : LanguageMapper<SrcL, SrcA, DstL, DstA> {
  DstL mapNode(const SrcL &node) const override { return DstL(node); }

  typename DstL::Discriminant mapDiscriminant(
      const typename SrcL::Discriminant &discriminant) const override {
    if constexpr (std::is_constructible_v<typename DstL::Discriminant,
                                          typename SrcL::Discriminant>) {
      return typename DstL::Discriminant(discriminant);
    } else {
      return static_cast<typename DstL::Discriminant>(discriminant);
    }
  }

  DstA mapAnalysis(const SrcA &analysis) const override {
    if constexpr (std::is_constructible_v<DstA, SrcA>) {
      return DstA(analysis);
    } else {
      (void)analysis;
      return DstA{};
    }
  }

  typename DstA::Data mapData(const typename SrcA::Data &data) const override {
    if constexpr (std::is_constructible_v<typename DstA::Data,
                                          typename SrcA::Data>) {
      return typename DstA::Data(data);
    } else {
      (void)data;
      return typename DstA::Data{};
    }
  }
};

template <typename L, typename AnalysisT>
inline Id EGraph<L, AnalysisT>::addInstantiation(const PatternAst<L> &pat,
                                                 const Subst &subst) {
  return find(addInstantiationNoncanonical(pat, subst));
}

template <typename L, typename AnalysisT>
inline std::pair<Id, bool> EGraph<L, AnalysisT>::unionInstantiations(
    const PatternAst<L> &from_pat, const PatternAst<L> &to_pat,
    const Subst &subst, const Symbol &reason) {
  Id from = addInstantiationNoncanonical(from_pat, subst);
  Id to = addInstantiationNoncanonical(to_pat, subst);
  return uniteImpl(from, to, Justification::ruleJustification(reason));
}

template <typename L, typename AnalysisT>
inline std::pair<Pattern<L>, Subst> EGraph<L, AnalysisT>::idToPattern(
    Id id, const std::unordered_map<Id, Id> &substitutions) const {
  RecExpr<ENodeOrVar<L>> pat;
  Subst subst;
  std::unordered_map<Id, Id> cache;
  idToPatternInternal(pat, id, substitutions, subst, cache);
  return {Pattern<L>(std::move(pat)), subst};
}

template <typename L, typename AnalysisT>
inline Id
EGraph<L, AnalysisT>::addInstantiationNoncanonical(const PatternAst<L> &pat,
                                                   const Subst &subst) {
  std::vector<Id> ids;
  ids.reserve(pat.size());
  for (const auto &node : pat.items()) {
    if (node.isVar()) {
      ids.push_back(find(subst.at(node.var())));
    } else {
      ids.push_back(addUncanonical(
          node.node().mapChildren([&](Id id) { return ids[id.index()]; })));
    }
  }
  return ids.back();
}

template <typename L, typename AnalysisT>
inline Id EGraph<L, AnalysisT>::idToPatternInternal(
    RecExpr<ENodeOrVar<L>> &pat, Id id,
    const std::unordered_map<Id, Id> &substitutions, Subst &subst,
    std::unordered_map<Id, Id> &cache) const {
  if (auto cached = cache.find(id); cached != cache.end()) {
    return cached->second;
  }

  if (auto sub_it = substitutions.find(id); sub_it != substitutions.end()) {
    Var var("?" + std::to_string(id.value()));
    subst.insert(var, sub_it->second);
    Id added = pat.add(ENodeOrVar<L>(var));
    cache.emplace(id, added);
    return added;
  }

  const auto &node = nodes_.at(id.index());
  std::vector<Id> children;
  children.reserve(node.children().size());
  for (Id child : node.children()) {
    children.push_back(
        idToPatternInternal(pat, child, substitutions, subst, cache));
  }
  auto rebuilt =
      node.mapChildren([&](Id child) { return children[child.index()]; });
  Id added = pat.add(ENodeOrVar<L>(rebuilt));
  cache.emplace(id, added);
  return added;
}

} // namespace lotus::egraph
