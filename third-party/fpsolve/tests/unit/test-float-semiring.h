#ifndef TEST_FLOAT_SEMIRING_H
#define TEST_FLOAT_SEMIRING_H

#include "fpsolve/semirings/float-semiring.h"
#include <gtest/gtest.h>

class FloatSemiringTest : public ::testing::Test
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
	FloatSemiring *null, *one, *first, *second;
};

#endif
