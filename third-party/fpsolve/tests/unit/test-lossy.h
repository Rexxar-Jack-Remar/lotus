/*
 * test-lossy.h
 *
 *  Created on: 18.05.2015
 *      Author: schlund
 */

#ifndef TEST_LOSSY_H_
#define TEST_LOSSY_H_


#include <gtest/gtest.h>

#include "fpsolve/semirings/lossy-finite-automaton.h"

class LossySemiringTest : public ::testing::Test
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
  LossyFiniteAutomaton *a, *b, *c;
};



#endif /* TEST_LOSSY_H_ */
