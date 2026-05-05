#ifndef TEST_POLYNOMIAL_H
#define TEST_POLYNOMIAL_H

#include <gtest/gtest.h>

#include "fpsolve/semirings/commutativeRExp.h"
#include "fpsolve/semirings/free-semiring.h"
#include "fpsolve/semirings/semilinSetNdd.h"
#include "fpsolve/semirings/why-set.h"

#include "fpsolve/datastructs/matrix.h"
#include "fpsolve/matrix_free_semiring.h"
#include "fpsolve/polynomials/commutative_polynomial.h"

//TODO: use here other semirings that are more informative...
#define TEST_SR WhySemiring

class PolynomialTest : public ::testing::Test
{
public:
	void SetUp() override;
	void TearDown() override;

protected:
  void testSemiring();
	void testAddition();
	void testMultiplication();
	void testJacobian();
	void testEvaluation();
	void testMatrixEvaluation();
	void testPolynomialToFreeSemiring();
	void testDerivativeBinomAt();

protected:
	TEST_SR *a, *b, *c, *d, *e;
	CommutativePolynomial<TEST_SR> *null, *one, *first, *second, *third, *p1;
};

#endif
