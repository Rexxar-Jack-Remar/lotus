#ifndef TEST_BOOL_SEMIRING_H
#define TEST_BOOL_SEMIRING_H

#include "fpsolve/semirings/bool-semiring.h"
#include <gtest/gtest.h>

class BoolSemiringTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
  void testSemiring();
	void testStar();
};

#endif
