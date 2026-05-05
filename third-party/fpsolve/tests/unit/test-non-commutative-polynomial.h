#pragma once
#include <gtest/gtest.h>

#include "fpsolve/semirings/free-semiring.h"
#include "fpsolve/datastructs/matrix.h"
#include "fpsolve/matrix_free_semiring.h"
#include "fpsolve/polynomials/non_commutative_polynomial.h"

class NonCommutativePolynomialTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
  void testSemiring();
	void testAddition();
	void testMultiplication();
	void testEvaluation();
	void testMatrixEvaluation();
	void testNonCommutativePolynomialToFreeSemiring();
	void testDerivative();

protected:
        NonCommutativePolynomial<FreeSemiring> *a, *b, *c, *d, *e;
	NonCommutativePolynomial<FreeSemiring> *null, *one, *first, *second, *p1, *X, *Y;
};
