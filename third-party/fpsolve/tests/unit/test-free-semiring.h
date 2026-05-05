#ifndef TEST_FREE_SEMIRING_H
#define TEST_FREE_SEMIRING_H

#include <gtest/gtest.h>

#include "fpsolve/semirings/free-semiring.h"

class FreeSemiringTest : public ::testing::Test
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
	FreeSemiring *a, *b, *c;
};

#endif
