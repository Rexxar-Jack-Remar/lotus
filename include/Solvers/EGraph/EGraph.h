#pragma once

#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EClass.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/UnionFind.h"

namespace lotus::egraph {

struct UnionEvent {
  Id left;
  Id right;
  std::string reason;
};

template <typename L, typename AnalysisT> class EGraph {
public:
  using Analysis = AnalysisT;
  using Data = typename AnalysisT::Data;
  using Class = EClass<L, Data>;

  EGraph() = default;
  explicit EGraph(AnalysisT analysis) : analysis_(std::move(analysis)) {}

  AnalysisT &analysis() { return analysis_; }
  const AnalysisT &analysis() const { return analysis_; }

  Id add(const L &node) {
    clean_ = false;
    L canonical = canonicalize(node);
    auto memo_it = memo_.find(canonical);
    if (memo_it != memo_.end()) {
      return find(memo_it->second);
    }

    Id id = union_find_.makeSet();
    memo_.emplace(canonical, id);

    Class klass{id, {canonical}, AnalysisT::make(*this, canonical, id), {}};
    classes_.emplace(id, std::move(klass));
    classes_by_op_[canonical.discriminant()].insert(id);

    for (Id child : canonical.children()) {
      classes_.at(find(child)).parents.push_back(id);
    }
    return id;
  }

  Id addExpr(const RecExpr<L> &expr) {
    std::vector<Id> ids;
    ids.reserve(expr.size());
    for (const auto &node : expr.items()) {
      ids.push_back(add(node.mapChildren([&](Id id) { return ids[id.index()]; })));
    }
    return ids.back();
  }

  std::optional<Id> lookup(const L &node) const {
    L canonical = canonicalize(node);
    auto it = memo_.find(canonical);
    if (it == memo_.end()) {
      return std::nullopt;
    }
    return find(it->second);
  }

  Id find(Id id) const { return union_find_.find(id); }
  Id findMut(Id id) { return union_find_.findMut(id); }

  Class &operator[](Id id) { return classes_.at(findMut(id)); }
  const Class &operator[](Id id) const { return classes_.at(find(id)); }

  size_t totalSize() const { return memo_.size(); }
  size_t numberOfClasses() const { return classes_.size(); }
  bool empty() const { return classes_.empty(); }
  bool clean() const { return clean_; }

  std::vector<Id> classIds() const {
    std::vector<Id> ids;
    ids.reserve(classes_.size());
    for (const auto &[id, _] : classes_) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  std::vector<std::reference_wrapper<const Class>> classes() const {
    std::vector<std::reference_wrapper<const Class>> out;
    for (const auto &[_, klass] : classes_) {
      out.emplace_back(klass);
    }
    std::sort(out.begin(), out.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.get().id < rhs.get().id; });
    return out;
  }

  std::vector<Id> classesForOp(const typename L::Discriminant &op) const {
    std::vector<Id> ids;
    auto it = classes_by_op_.find(op);
    if (it == classes_by_op_.end()) {
      return ids;
    }
    ids.insert(ids.end(), it->second.begin(), it->second.end());
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  Id unite(Id lhs, Id rhs, const std::string &reason = {}) {
    lhs = findMut(lhs);
    rhs = findMut(rhs);
    if (lhs == rhs) {
      return lhs;
    }

    if (rhs < lhs) {
      std::swap(lhs, rhs);
    }

    analysis_.preUnion(*this, lhs, rhs, reason.empty() ? nullptr : &reason);
    if (explanations_enabled_) {
      union_events_.push_back({lhs, rhs, reason});
    }
    union_find_.unite(lhs, rhs);

    auto &left_class = classes_.at(lhs);
    auto right_it = classes_.find(rhs);
    if (right_it != classes_.end()) {
      auto right_class = std::move(right_it->second);
      auto merge_result = analysis_.merge(left_class.data, std::move(right_class.data));
      (void)merge_result;
      left_class.nodes.insert(left_class.nodes.end(), right_class.nodes.begin(),
                              right_class.nodes.end());
      left_class.parents.insert(left_class.parents.end(), right_class.parents.begin(),
                                right_class.parents.end());
      classes_.erase(right_it);
    }

    analysis_.modify(*this, lhs);
    clean_ = false;
    return lhs;
  }

  void setAnalysisData(Id id, Data data) {
    (*this)[id].data = std::move(data);
    analysis_.modify(*this, findMut(id));
  }

  void rebuild() {
    bool changed = false;
    do {
      changed = false;

      for (auto &[_, klass] : classes_) {
        for (auto &node : klass.nodes) {
          for (Id &child : node.childrenMut()) {
            child = findMut(child);
          }
        }

        std::unordered_set<L> unique_nodes;
        std::vector<L> deduped;
        deduped.reserve(klass.nodes.size());
        for (const auto &node : klass.nodes) {
          if (unique_nodes.insert(node).second) {
            deduped.push_back(node);
          }
        }
        klass.nodes = std::move(deduped);
      }

      memo_.clear();
      classes_by_op_.clear();

      auto ids = classIds();
      for (Id id : ids) {
        auto &klass = classes_.at(id);
        for (const auto &node : klass.nodes) {
          auto memo_it = memo_.find(node);
          if (memo_it == memo_.end()) {
            memo_.emplace(node, id);
            classes_by_op_[node.discriminant()].insert(id);
            continue;
          }

          Id other = findMut(memo_it->second);
          if (other != id) {
            unite(other, id, "congruence");
            changed = true;
            break;
          }
        }
      }
    } while (changed);

    recomputeParents();
    recomputeAnalysis();
    clean_ = true;
  }

  EGraph withExplanationsEnabled() const {
    EGraph copy = *this;
    copy.explanations_enabled_ = true;
    return copy;
  }

  bool areExplanationsEnabled() const { return explanations_enabled_; }
  const std::vector<UnionEvent> &unionEvents() const { return union_events_; }

private:
  L canonicalize(const L &node) const {
    auto copy = node;
    for (Id &child : copy.childrenMut()) {
      child = find(child);
    }
    return copy;
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

  void recomputeAnalysis() {
    for (Id id : classIds()) {
      auto &klass = classes_.at(id);
      std::optional<Data> merged;
      for (const auto &node : klass.nodes) {
        Data data = AnalysisT::remake(*this, node, id);
        if (!merged) {
          merged = std::move(data);
        } else {
          analysis_.merge(*merged, std::move(data));
        }
      }
      if (merged) {
        klass.data = std::move(*merged);
      }
    }
  }

  AnalysisT analysis_;
  UnionFind union_find_;
  std::unordered_map<L, Id> memo_;
  std::unordered_map<Id, Class> classes_;
  std::unordered_map<typename L::Discriminant, std::unordered_set<Id>> classes_by_op_;
  std::vector<UnionEvent> union_events_;
  bool clean_ = false;
  bool explanations_enabled_ = false;
};

} // namespace lotus::egraph
