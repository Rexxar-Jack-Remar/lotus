#pragma once

#include "Solvers/EGraph.h"

#if LOTUS_EGRAPH_ENABLE_JSON
#include "Utils/Formats/json11.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using namespace lotus::egraph;

namespace {

struct NoCycleAnalysis : NoAnalysis<SymbolLang> {
  bool allowEMatchingCycles() const { return false; }
};

struct BorrowedSearcher final : Searcher<SymbolLang> {
  explicit BorrowedSearcher(const Pattern<SymbolLang> &pattern)
      : pattern(pattern) {}

  std::optional<SearchMatches<SymbolLang>>
  searchEClassWithLimit(const EGraph<SymbolLang> &egraph, Id eclass,
                        size_t limit,
                        const WorkControl *control = nullptr) const override {
    return pattern.searchEClassWithLimit(egraph, eclass, limit, control);
  }

  std::vector<Var> vars() const override { return pattern.vars(); }

  const PatternAst<SymbolLang> *getPatternAst() const override {
    return &pattern.ast();
  }

  const Pattern<SymbolLang> &pattern;
};

struct BorrowedVarCondition {
  explicit BorrowedVarCondition(const Var &required) : required(required) {}

  bool operator()(EGraph<SymbolLang> &, Id, const Subst &) const {
    return true;
  }

  std::vector<Var> vars() const { return {required}; }

  const Var &required;
};

} // namespace

namespace {

std::vector<Rewrite<SymbolLang>> makeExplanationRules() {
  return {
      makeRewrite<SymbolLang>("commute-add", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang>("commute-mul", "(* ?a ?b)", "(* ?b ?a)"),
      makeRewrite<SymbolLang>("assoc-add", "(+ ?a (+ ?b ?c))",
                              "(+ (+ ?a ?b) ?c)"),
      makeRewrite<SymbolLang>("add-0", "(+ ?a 0)", "?a"),
      makeRewrite<SymbolLang>("mul-1", "(* ?a 1)", "?a"),
      makeRewrite<SymbolLang>("factor", "(+ ?a ?a)", "(* 2 ?a)"),
      makeRewrite<SymbolLang>("cancel-sub", "(- ?a ?a)", "0"),
  };
}

} // namespace

namespace {

struct CountingCostFn : CostFunction<CountingCostFn, SymbolLang, size_t> {
  using Cost = typename CostFunction<CountingCostFn, SymbolLang, size_t>::Cost;
  size_t calls = 0;

  template <typename ChildCostFn>
  Cost cost(const SymbolLang &node, ChildCostFn &&child_cost) {
    ++calls;
    size_t total = node.op() == "cheap" ? 1 : 5;
    for (Id child : node.children()) {
      total = std::min(std::numeric_limits<size_t>::max(),
                       total + child_cost(child));
    }
    return total;
  }
};

struct SharedCountingCostFn
    : CostFunction<SharedCountingCostFn, SymbolLang, size_t> {
  using Cost = size_t;

  explicit SharedCountingCostFn(std::shared_ptr<size_t> calls)
      : calls(std::move(calls)) {}

  template <typename ChildCostFn>
  Cost cost(const SymbolLang &node, ChildCostFn &&child_cost) {
    ++*calls;
    size_t total = 1;
    for (Id child : node.children()) {
      total += child_cost(child);
    }
    return total;
  }

  std::shared_ptr<size_t> calls;
};

} // namespace

namespace {

struct DedupAnalysis {
  using Data = int;

  inline static int remake_calls = 0;

  static void reset() { remake_calls = 0; }

  static Data make(EGraph<SymbolLang, DedupAnalysis> &, const SymbolLang &,
                   Id) {
    return 0;
  }

  static Data remake(EGraph<SymbolLang, DedupAnalysis> &, const SymbolLang &,
                     Id) {
    ++remake_calls;
    return 0;
  }

  void preUnion(const EGraph<SymbolLang, DedupAnalysis> &, Id, Id,
                const std::optional<Justification> &) const {}
  DidMerge merge(Data &, Data) { return {}; }
  void modify(EGraph<SymbolLang, DedupAnalysis> &, Id) const {}
  bool allowEMatchingCycles() const { return true; }
};

} // namespace

LOTUS_EGRAPH_DEFINE_LANGUAGE(
    TestLang, {LOTUS_EGRAPH_LANG_OP("+", 2) LOTUS_EGRAPH_LANG_OP("*", 2)
                   LOTUS_EGRAPH_LANG_OP("x", 0) LOTUS_EGRAPH_LANG_OP("y", 0)});
