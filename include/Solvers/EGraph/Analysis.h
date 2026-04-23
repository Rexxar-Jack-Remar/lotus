#pragma once

#include "Solvers/EGraph/Id.h"

#include <optional>
#include <variant>

namespace lotus::egraph {

struct DidMerge {
  bool left_changed = false;
  bool right_changed = false;
};

template <typename L> struct NoAnalysis;
template <typename L, typename AnalysisT = NoAnalysis<L>> class EGraph;

template <typename L> struct NoAnalysis {
  using Data = std::monostate;

  template <typename AnyAnalysis>
  static Data make(EGraph<L, AnyAnalysis> &, const L &, Id) {
    return {};
  }

  template <typename AnyAnalysis>
  static Data remake(EGraph<L, AnyAnalysis> &, const L &, Id) {
    return {};
  }

  template <typename AnyAnalysis>
  void preUnion(const EGraph<L, AnyAnalysis> &, Id, Id,
                const std::string *) const {}

  DidMerge merge(Data &, Data) { return {}; }

  template <typename AnyAnalysis>
  void modify(EGraph<L, AnyAnalysis> &, Id) const {}

  bool allowEMatchingCycles() const { return true; }
};

template <typename T>
inline DidMerge mergeMax(T &target, T source) {
  if (source > target) {
    target = std::move(source);
    return {true, false};
  }
  return {false, target != source};
}

template <typename T, typename F>
inline DidMerge mergeOption(std::optional<T> &target, std::optional<T> source,
                            F &&on_both) {
  if (!source) {
    return {false, false};
  }
  if (!target) {
    target = std::move(source);
    return {true, false};
  }
  return on_both(*target, *source);
}

} // namespace lotus::egraph
