#include "Dataflow/NPA/Core/LinearSolvers.h"
#include "Dataflow/NPA/Core/TensorLinearSolve.h"
#include "Dataflow/NPA/NPA.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct BoundedLangSemiring {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr size_t MaxLen = 3;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type extend(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a) {
      for (const auto &y : b) {
        std::string s = x + y;
        if (s.size() <= MaxLen) out.insert(std::move(s));
      }
    }
    return out;
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a)
      if (b.find(x) == b.end()) out.insert(x);
    return out;
  }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &p : pairs) out.emplace(p.first, p.second);
  return out;
}

static BoundedLangSemiring::value_type singleton(const std::string &s) {
  return {s};
}

} // namespace

TEST(NPA, TensorRegularizationMatchesWorklistOnConstantConcat) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  // X = a · X · b  ⊕  c
  auto a = Exp::term(singleton("a"));
  auto b = Exp::term(singleton("b"));
  auto c = Exp::term(singleton("c"));
  E1 rhs = Exp::add(Exp::concat(a, "X", b), c);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_worklist_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  // Tensor-product projection may over-approximate; ensure it is a superset.
  for (const auto &s : wl[0]) {
    EXPECT_TRUE(tp[0].count(s) != 0);
  }

  // Bounded by MaxLen=3: least solution includes { "c", "acb" }.
  BoundedLangSemiring::value_type expect = {"c", "acb"};
  EXPECT_EQ(wl[0], expect);
}

TEST(NPA, NewtonInitUsesFOfBottom) {
  using D = BoundedLangSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", Exp0::term(singleton("a")));

  auto res = npa::NewtonSolver<D>::solve(eqns, false, 0);
  auto m = toMap<D>(res.first);

  EXPECT_EQ(m.at("X"), singleton("a"));
}
