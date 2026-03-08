#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/NPA.h"

#include <gtest/gtest.h>

namespace {

using D = npa::PredicateRelationDomain;
using TD = npa::PredicateTensorDomain;

std::vector<std::pair<std::uint64_t, std::uint64_t>>
sortedTransitions(const D::value_type &relation) {
  auto transitions = D::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

} // namespace

TEST(NPA, PredicateRelationIdentityAndAssignmentCompose) {
  D::configure(1);

  auto id = D::one();
  auto set_true = D::assignConst(0, true);
  auto assume_false = D::assume(0, false);
  auto composed = D::extend(set_true, assume_false);

  EXPECT_EQ(sortedTransitions(id),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 0}, {1, 1}}));
  EXPECT_TRUE(sortedTransitions(composed).empty());
}

TEST(NPA, PredicateRelationTensorRegularizationMatchesWorklist) {
  D::configure(1);

  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto set_true = Exp::term(D::assignConst(0, true));
  auto assume_false = Exp::term(D::assume(0, false));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::add(Exp::concat(set_true, "X", assume_false), id);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_worklist_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_TRUE(D::equal(wl[0], tp[0]));
  EXPECT_EQ(sortedTransitions(tp[0]),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 0}, {1, 1}}));
}

TEST(NPA, PredicateTensorReadoutMatchesBaseExtend) {
  D::configure(1);

  auto lhs = D::assignConst(0, true);
  auto rhs = D::assume(0, false);

  auto coupled = TD::couple(lhs, rhs);
  auto readout = TD::readout(coupled);

  EXPECT_TRUE(D::equal(readout, D::extend(lhs, rhs)));
}

TEST(NPA, PredicateTensorReadoutAvoidsCrossTermsAcrossAlternatives) {
  D::configure(1);

  auto set_true = D::assignConst(0, true);
  auto identity = D::one();
  auto assume_false = D::assume(0, false);

  auto coupled = TD::combine(TD::couple(identity, set_true),
                             TD::couple(assume_false, identity));
  auto readout = TD::readout(coupled);

  auto expected =
      D::combine(D::extend(identity, set_true), D::extend(assume_false, identity));
  EXPECT_TRUE(D::equal(readout, expected));
}

TEST(NPA, PredicateRelationProjectElidesLocalUpdates) {
  D::configure(2, 1);

  auto set_local_true = D::assignConst(1, true);
  auto projected = D::project(set_local_true);

  EXPECT_EQ(sortedTransitions(projected),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}, {2, 2}, {3, 3}}));
}

TEST(NPA, PredicateRelationMergeUsesProjectedSecondOperand) {
  D::configure(2, 1);

  auto set_global_true = D::assignConst(0, true);
  auto set_local_true = D::assignConst(1, true);
  auto merged = D::merge(set_global_true, set_local_true);

  EXPECT_TRUE(D::equal(merged, set_global_true));
}

TEST(NPA, PredicateTensorProjectTSatisfiesLemma88Laws) {
  D::configure(2, 1);

  auto a = TD::couple(D::assignConst(0, true), D::one());
  auto b = TD::couple(D::one(), D::assignConst(1, true));
  auto c = TD::couple(D::assignConst(1, true), D::assignConst(0, true));

  EXPECT_TRUE(TD::equal(TD::projectT(TD::combine(a, b)),
                        TD::combine(TD::projectT(a), TD::projectT(b))));
  EXPECT_TRUE(TD::equal(TD::projectT(TD::projectT(c)), TD::projectT(c)));

  auto lhs = TD::extend(TD::projectT(a), TD::projectT(b));
  auto rhs1 = TD::projectT(TD::extend(a, TD::projectT(b)));
  auto rhs2 = TD::projectT(TD::extend(TD::projectT(a), b));

  EXPECT_TRUE(TD::equal(lhs, rhs1));
  EXPECT_TRUE(TD::equal(lhs, rhs2));
}
