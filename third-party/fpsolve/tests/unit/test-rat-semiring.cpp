/*
 * test-rat-semiring.cpp
 *
 *  Created on: 23.06.2015
 *      Author: schlund
 */

#include "test-rat-semiring.h"
#include "util.h"

void RatSemiringTest::SetUp()
{
  std::cout << "Prec-Rat-SR-Test :" << std::endl;
  null = new PrecRatSemiring("0");
  one = new PrecRatSemiring("1");
  first = new PrecRatSemiring("2/3");
  second = new PrecRatSemiring("23/5");
}

void RatSemiringTest::TearDown()
{
  delete null;
  delete one;
  delete first;
  delete second;
}


TEST_F(RatSemiringTest, testSemiring)
{
  generic_test_semiring(*first, *second);
}

TEST_F(RatSemiringTest, testAddition)
{
  EXPECT_TRUE( (*first) + (*second) == PrecRatSemiring("79/15") );
}

TEST_F(RatSemiringTest, testMultiplication)
{
  EXPECT_TRUE( (*first) * (*second) == PrecRatSemiring("46/15") );
}

TEST_F(RatSemiringTest, testStar)
{
  EXPECT_TRUE( (*null).star() == *one );
  EXPECT_TRUE( PrecRatSemiring("1/2").star() == PrecRatSemiring(2) );
}


