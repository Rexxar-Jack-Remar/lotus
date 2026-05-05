#include "test-free-semiring.h"
#include "util.h"

void FreeSemiringTest::SetUp()
{
  std::cout << "FreeSR-Test :" << std::endl;
	a = new FreeSemiring(Var::GetVarId("a"));
	b = new FreeSemiring(Var::GetVarId("b"));
	c = new FreeSemiring(Var::GetVarId("c"));
}

void FreeSemiringTest::TearDown()
{
	delete a;
	delete b;
	delete c;
}

TEST_F(FreeSemiringTest, testSemiring)
{
  generic_test_semiring(*a,*b);
  EXPECT_TRUE(!( (*a) * (*b) == (*b) * (*a)));
}

TEST_F(FreeSemiringTest, testAddition)
{
	// a + 0 = a
	EXPECT_TRUE( (*a) + FreeSemiring::null() == (*a) );
	// 0 + a = a
	EXPECT_TRUE( FreeSemiring::null() + (*a) == (*a) );

	// associative (a + b) + c == a + (b + c)
	// EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(FreeSemiringTest, testMultiplication)
{
	// a . 1 = a
	EXPECT_TRUE( (*a) * FreeSemiring::one() == (*a) );
	// 1 . a = a
	EXPECT_TRUE( FreeSemiring::one() * (*a) == (*a) );
	// a . 0 = 0
	EXPECT_TRUE( (*a) * FreeSemiring::null() == FreeSemiring::null() );
	// 0 . a = 0
	EXPECT_TRUE( FreeSemiring::null() * (*a) == FreeSemiring::null() );

	// associative (a * b) * c == a * (b * c)
	// EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );
}

TEST_F(FreeSemiringTest, testStar)
{
	// 0* = 1
	EXPECT_TRUE( FreeSemiring::null().star() == FreeSemiring::one() );

        // FIXME: disabled because the new FreeSemiring does not have the
        // corresponding constructor...
	// EXPECT_TRUE( a->star() == FreeSemiring(FreeSemiring::Star, *a));
}
