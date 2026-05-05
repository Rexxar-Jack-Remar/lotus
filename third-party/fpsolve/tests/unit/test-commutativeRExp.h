#ifndef TEST_COMMUTATIVEREXP_H
#define TEST_COMMUTATIVEREXP_H

#include "fpsolve/semirings/commutativeRExp.h"
#include <gtest/gtest.h>

class CommutativeRExpTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
        void testSemiring();
	void testAddition();
	void testMultiplication();
	void testStar();
	void testTerms();

protected:
	CommutativeRExp *a, *b, *c;
};

#endif
