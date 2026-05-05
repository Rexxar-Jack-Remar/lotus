/*
 * test-why-set.h
 *
 *  Created on: 15.05.2014
 *      Author: schlund
 */

#ifndef TEST_WHY_SET_H_
#define TEST_WHY_SET_H_




include "fpsolve/semirings/why-set.h"
#include <gtest/gtest.h>

class WhySetSemiringTest : public ::testing::Test
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
  WhySemiring *a, *b,*c , *d;
};




#endif /* TEST_WHY_SET_H_ */
