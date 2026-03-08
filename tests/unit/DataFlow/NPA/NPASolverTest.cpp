#include "Dataflow/NPA/NPA.h"
#include "Dataflow/NPA/Domains/ProgramTransferDomain.h"

#include <gtest/gtest.h>

#include <unordered_map>

namespace {

struct BoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }

  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) { return extend(a, b); }
  static value_type ndetCombine(value_type a, value_type b) { return combine(a, b); }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &p : pairs) out.emplace(p.first, p.second);
  return out;
}

} // namespace

TEST(NPA, HoleCanReferenceOtherEquationVariable) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  // x = y
  // y = 1
  //
  // This exercises:
  // - Exp0::Hole lookup against ν in I0 (Kleene/Newton evaluation)
  // - Exp1::Hole lookup against the linear-solver environment in I1 (Newton step)
  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto kleeneRes = npa::KleeneSolver<D>::solve(eqns);
  auto kleeneMap = toMap<D>(kleeneRes.first);
  EXPECT_TRUE(kleeneMap.at("y"));
  EXPECT_TRUE(kleeneMap.at("x"));

  auto newtonRes = npa::NewtonSolver<D>::solve(eqns);
  auto newtonMap = toMap<D>(newtonRes.first);
  EXPECT_TRUE(newtonMap.at("y"));
  EXPECT_TRUE(newtonMap.at("x"));
}

namespace {

struct TraceSemiring {
  using value_type = std::string;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return "0"; }
  static value_type one() { return "1"; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    return "(" + a + "+" + b + ")";
  }
  static value_type extend(const value_type &a, const value_type &b) {
    return "(" + a + "*" + b + ")";
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return "(" + a + "-" + b + ")";
  }
};

struct WriteOp {
  const void *dest = nullptr;

  bool operator<(const WriteOp &other) const { return dest < other.dest; }
  bool operator==(const WriteOp &other) const { return dest == other.dest; }
};

} // namespace

TEST(NPA, ConcatRepresentsTwoSidedMultiplication) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "X";

  // A · x · B
  E e = Exp::concat(Exp::term("A"), "x", Exp::term("B"));
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "(A*(X*B))");
}

TEST(NPA, BoundVariablesDoNotAliasEquationVariables) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "GLOBAL";

  // inf(body, x) where body references x as a *bound* variable.
  // With our TraceSemiring, fixpoint starting at 0:
  //   cur0=0, body = (A*cur), so it stabilizes at "0" only if A is "1".
  // Use body = bound(x) so result should be the initial "0".
  E e = Exp::inf(Exp::bound("x"), "x");
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "0");
}

TEST(NPA, ProgramTransferDomainPreservesMayWriteAcrossCombineAndExtend) {
  using D = npa::ProgramTransferDomain<WriteOp>;

  static int slot_a = 0;
  static int slot_b = 0;

  auto a = D::singleton(WriteOp{&slot_a});
  auto b = D::singleton(WriteOp{&slot_b});

  auto joined = D::combine(a, b);
  EXPECT_TRUE(joined.may_write.count(&slot_a));
  EXPECT_TRUE(joined.may_write.count(&slot_b));

  auto composed = D::extend(a, b);
  EXPECT_TRUE(composed.may_write.count(&slot_a));
  EXPECT_TRUE(composed.may_write.count(&slot_b));
}
