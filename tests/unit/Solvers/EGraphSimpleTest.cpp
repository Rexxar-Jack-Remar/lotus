#include "Solvers/EGraph.h"

#include <gtest/gtest.h>

using namespace lotus::egraph;

namespace {

using EG = EGraph<SymbolLang>;
using RW = Rewrite<SymbolLang>;

std::vector<RW> makeRules() {
  return {
      makeRewrite<SymbolLang>("commute-add", "(+ ?a ?b)", "(+ ?b ?a)"),
      makeRewrite<SymbolLang>("commute-mul", "(* ?a ?b)", "(* ?b ?a)"),
      makeRewrite<SymbolLang>("add-0", "(+ ?a 0)", "?a"),
      makeRewrite<SymbolLang>("mul-0", "(* ?a 0)", "0"),
      makeRewrite<SymbolLang>("mul-1", "(* ?a 1)", "?a"),
  };
}

std::string simplify(std::string_view text) {
  RecExpr<SymbolLang> expr = RecExpr<SymbolLang>::parse(text);
  Runner<SymbolLang> runner;
  runner.withExpr(expr).run(makeRules());
  Extractor<SymbolLang> extractor(runner.egraph);
  return extractor.findBest(runner.roots.front()).second.toString();
}

} // namespace

TEST(EGraphSimpleTest, SimplifiesBasicExpressions) {
  EXPECT_EQ(simplify("(* 0 42)"), "0");
  EXPECT_EQ(simplify("(+ 0 (* 1 foo))"), "foo");
}
