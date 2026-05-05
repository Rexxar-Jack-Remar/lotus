/*
 * test-rat-semiring.h
 *
 *  Created on: 23.06.2015
 *      Author: schlund
 */

#ifndef TEST_RAT_SEMIRING_H_
#define TEST_RAT_SEMIRING_H_


#include "fpsolve/semirings/prec-rat-semiring.h"
#include <gtest/gtest.h>

class RatSemiringTest : public ::testing::Test
{

public:
  void SetUp() override;
  void TearDown() override;

protected:
  void testSemiring();
  void testAddition();
  void testMultiplication();
  void testStar();

protected:
  PrecRatSemiring *null, *one, *first, *second;
};



#endif /* TEST_RAT_SEMIRING_H_ */
