/*
 * test-viterbi.h
 *
 *  Created on: 22.07.2014
 *      Author: schlund
 */

#ifndef TEST_VITERBI_H_
#define TEST_VITERBI_H_

#include "fpsolve/semirings/viterbi-semiring.h"
#include <gtest/gtest.h>

class ViterbiSemiringTest : public ::testing::Test
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
  ViterbiSemiring *a, *b, *c, *d;
};



#endif /* TEST_VITERBI_H_ */
