#ifndef TEST_PREFIX_SEMIRING_H
#define TEST_PREFIX_SEMIRING_H

#include "fpsolve/semirings/float-semiring.h"
#include "fpsolve/semirings/bool-semiring.h"
#include "fpsolve/semirings/tuple-semiring.h"
#include <gtest/gtest.h>

class TupleSemiringTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
        void testSemiring();
	void testStar();

protected:
	TupleSemiring<FloatSemiring, BoolSemiring> *first, *second;
};

#endif
