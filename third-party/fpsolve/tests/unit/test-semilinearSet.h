/*
 * test-semilinearSet.h
 *
 *  Created on: 16.04.2014
 *      Author: schlund
 */

#ifndef TEST_SEMILINEARSET_H_
#define TEST_SEMILINEARSET_H_

#include <gtest/gtest.h>

#include "fpsolve/semirings/semilinear_set.h"


typedef SemilinSetExp SLSet;

class SemilinSetTest : public ::testing::Test
{

public:
  void SetUp() override;
  void TearDown() override;

protected:
  void testSemiring();
  void testBasic();
  void testAddition();
  void testMultiplication();
  void testStar();
  void testTerms();

protected:
  SLSet *a, *b, *c, *d, *e;
};




#endif /* TEST_SEMILINEARSET_H_ */
