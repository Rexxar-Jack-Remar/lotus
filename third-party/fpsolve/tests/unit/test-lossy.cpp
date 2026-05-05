/*
 * test-lossy.cpp
 *
 *  Created on: 18.05.2015
 *      Author: schlund
 */

#include "test-lossy.h"
#include "util.h"

void LossySemiringTest::SetUp()
{
  std::cout << "Lossy-FA-Test :" << '\n';
  a = new LossyFiniteAutomaton(Var::GetVarId("a"));
  b = new LossyFiniteAutomaton(Var::GetVarId("b"));
  c = new LossyFiniteAutomaton(Var::GetVarId("c"));
}

void LossySemiringTest::TearDown()
{
  delete a;
  delete b;
  delete c;
}

TEST_F(LossySemiringTest, testSemiring)
{
  generic_test_semiring(*a,*b);
  EXPECT_TRUE(!( (*a) * (*b) == (*b) * (*a)));
}

TEST_F(LossySemiringTest, testAddition)
{
  // a + 0 = a
  EXPECT_TRUE( (*a) + LossyFiniteAutomaton::null() == (*a) );
  // 0 + a = a
  EXPECT_TRUE( LossyFiniteAutomaton::null() + (*a) == (*a) );

  // associativity (a + b) + c == a + (b + c)
  EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(LossySemiringTest, testMultiplication)
{
  // a . 1 = a
  EXPECT_TRUE( (*a) * LossyFiniteAutomaton::one() == (*a) );
  // 1 . a = a
  EXPECT_TRUE( LossyFiniteAutomaton::one() * (*a) == (*a) );
  // a . 0 = 0
  EXPECT_TRUE( (*a) * LossyFiniteAutomaton::null() == LossyFiniteAutomaton::null() );
  // 0 . a = 0
  EXPECT_TRUE( LossyFiniteAutomaton::null() * (*a) == LossyFiniteAutomaton::null() );

  // associativity (a * b) * c == a * (b * c)
  EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );
}

TEST_F(LossySemiringTest, testStar)
{
  // 0* = 1
  EXPECT_TRUE( LossyFiniteAutomaton::null().star() == LossyFiniteAutomaton::one() );

        // FIXME: disabled because the new FreeSemiring does not have the
        // corresponding constructor...
  // EXPECT_TRUE( a->star() == FreeSemiring(FreeSemiring::Star, *a));
}


