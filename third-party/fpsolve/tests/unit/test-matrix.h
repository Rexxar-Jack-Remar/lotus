#ifndef TEST_MATRIX_H_
#define TEST_MATRIX_H_

#include <gtest/gtest.h>

#include "fpsolve/semirings/free-semiring.h"




#include "fpsolve/datastructs/matrix.h"
#include "fpsolve/semirings/float-semiring.h"
#include "fpsolve/semirings/prec-rat-semiring.h"
#include "fpsolve/semirings/tropical-semiring.h"


class MatrixTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
	void testAddition();
	void testMultiplication();
	void testStar();

protected:
	FreeSemiring *a, *b, *c, *d, *e, *f, *g, *h, *i, *j, *k, *l, *m, *n, *o, *p, *q, *r;
	Matrix<FreeSemiring> *null, *one, *first, *second, *third, *fourth, *A;
};

#endif
