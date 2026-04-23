#include "Solvers/EGraph.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>

using namespace lotus::egraph;

namespace {

double parseNumericLeaf(const SymbolLang &node) {
  if (!node.children().empty()) {
    throw std::runtime_error("Expected numeric leaf");
  }
  return std::stod(node.op());
}

struct ConstantFold {
  using Data = std::optional<double>;

  static Data make(EGraph<SymbolLang, ConstantFold> &egraph, const SymbolLang &node,
                   Id) {
    if (node.children().empty()) {
      try {
        return parseNumericLeaf(node);
      } catch (...) {
        return std::nullopt;
      }
    }

    auto get = [&](Id child) -> std::optional<double> {
      return egraph[child].data;
    };

    if (node.op() == "+" && node.children().size() == 2) {
      auto lhs = get(node.children()[0]);
      auto rhs = get(node.children()[1]);
      if (lhs && rhs) {
        return *lhs + *rhs;
      }
    }
    if (node.op() == "*" && node.children().size() == 2) {
      auto lhs = get(node.children()[0]);
      auto rhs = get(node.children()[1]);
      if (lhs && rhs) {
        return *lhs * *rhs;
      }
    }
    return std::nullopt;
  }

  static Data remake(EGraph<SymbolLang, ConstantFold> &egraph,
                     const SymbolLang &node, Id id) {
    return make(egraph, node, id);
  }

  void preUnion(const EGraph<SymbolLang, ConstantFold> &, Id, Id,
                const Symbol *) const {}

  DidMerge merge(Data &target, Data source) {
    return mergeOption(target, source, [](double &lhs, double rhs) {
      EXPECT_NEAR(lhs, rhs, 1e-9);
      return DidMerge{false, false};
    });
  }

  void modify(EGraph<SymbolLang, ConstantFold> &egraph, Id id) const {
    auto data = egraph[id].data;
    if (!data) {
      return;
    }

    auto added = egraph.add(SymbolLang::leaf(std::to_string(*data)));
    egraph.unite(id, added, "constant_fold");
  }

  bool allowEMatchingCycles() const { return true; }
};

using RW = Rewrite<SymbolLang, ConstantFold>;

Condition<SymbolLang, ConstantFold> isNotZero(std::string_view name) {
  Var var = Var::parse(name);
  return [var](EGraph<SymbolLang, ConstantFold> &egraph, Id,
               const Subst &subst) -> bool {
    auto data = egraph[subst.at(var)].data;
    return !data || std::fabs(*data) > 1e-9;
  };
}

std::vector<RW> rules() {
  return {
      makeRewrite<SymbolLang, ConstantFold>("comm-add", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang, ConstantFold>("comm-mul", "(* ?a ?b)", "(* ?b ?a)"),
      makeRewrite<SymbolLang, ConstantFold>("assoc-add", "(+ ?a (+ ?b ?c))",
                                            "(+ (+ ?a ?b) ?c)"),
      makeRewrite<SymbolLang, ConstantFold>("zero-add", "(+ ?a 0)", "?a"),
      makeRewrite<SymbolLang, ConstantFold>("zero-mul", "(* ?a 0)", "0"),
      makeRewrite<SymbolLang, ConstantFold>("one-mul", "(* ?a 1)", "?a"),
      makeRewrite<SymbolLang, ConstantFold>("pow1", "(pow ?x 1)", "?x"),
      makeRewrite<SymbolLang, ConstantFold>("pow-recip", "(pow ?x -1)", "(/ 1 ?x)",
                                            {isNotZero("?x")}),
  };
}

std::string simplify(std::string_view text) {
  Runner<SymbolLang, ConstantFold> runner;
  runner.withExpr(RecExpr<SymbolLang>::parse(text)).withIterLimit(12).run(rules());
  Extractor<SymbolLang, ConstantFold> extractor(runner.egraph);
  return extractor.findBest(runner.roots.front()).second.toString();
}

} // namespace

TEST(EGraphMathTest, ConstantFoldAndSimplify) {
  EXPECT_EQ(simplify("(+ 1 (* 2 3))"), "7.000000");
}

TEST(EGraphMathTest, AlgebraicRewriteStillWorksWithAnalysis) {
  EXPECT_EQ(simplify("(* 1 (+ 0 x))"), "x");
}
