/*
 * test-semilinSetNdd.h
 *
 *  Created on: 28.01.2014
 *      Author: Michael Kerscher
 */

#ifndef TEST_SEMILINSETNDD_H_
#define TEST_SEMILINSETNDD_H_

#include <gtest/gtest.h>

#include "fpsolve/semirings/semilinSetNdd.h"


class SemilinSetNddTest : public ::testing::Test
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
	SemilinSetNdd *a, *b, *c;
};



#endif /* TEST_SEMILINSETNDD_H_ */
