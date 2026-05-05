#ifndef TEST_PREFIX_SEMIRING_H
#define TEST_PREFIX_SEMIRING_H

#include "fpsolve/semirings/prefix-semiring.h"
#include <gtest/gtest.h>

class PrefixSemiringTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
        void testSemiring();
	void testStar();

protected:
	PrefixSemiring *first, *second;
};

#endif
