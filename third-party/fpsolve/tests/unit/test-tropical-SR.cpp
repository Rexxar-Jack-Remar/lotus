/*
 * test-tropical-SR.cpp
 *
 *  Created on: 14.03.2013
 *      Author: schlund
 */

#include "test-tropical-SR.h"
#include "util.h"

void TropicalSemiringTest::SetUp()
{
  std::cout << "Tropical-SR-Test :" << '\n';
  first = new TropicalSemiring(2);
  second = new TropicalSemiring(5);
}

void TropicalSemiringTest::TearDown()
{
  delete first;
  delete second;
}

TEST_F(TropicalSemiringTest, testSemiring)
{
  generic_test_semiring(*first, *second);
}

TEST_F(TropicalSemiringTest, testAddition)
{
  EXPECT_TRUE( (*first) + (*second) == (*first));
  EXPECT_TRUE( (*first) + TropicalSemiring::null() == (*first));
  EXPECT_TRUE( TropicalSemiring::null() + (*first) == (*first));
}

TEST_F(TropicalSemiringTest, testMultiplication)
{
  EXPECT_TRUE( (*first) * (*second) == TropicalSemiring(2 + 5) );
}

TEST_F(TropicalSemiringTest, testStar)
{
  EXPECT_TRUE( TropicalSemiring::null().star() == TropicalSemiring::one() );
  EXPECT_TRUE( TropicalSemiring(3).star() == TropicalSemiring(0) );
}
