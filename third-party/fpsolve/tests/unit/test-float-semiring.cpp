#include "test-float-semiring.h"
#include "util.h"

void FloatSemiringTest::SetUp()
{
  std::cout << "Float-SR-Test :" << std::endl;
	null = new FloatSemiring(0.0);
	one = new FloatSemiring(1.0);
	first = new FloatSemiring(1.2);
	second = new FloatSemiring(4.3);
}

void FloatSemiringTest::TearDown()
{
	delete null;
	delete one;
	delete first;
	delete second;
}

TEST_F(FloatSemiringTest, testSemiring)
{
  generic_test_semiring(*first, *second);
}

TEST_F(FloatSemiringTest, testAddition)
{
	EXPECT_TRUE( (*first) + (*second) == FloatSemiring( 1.2 + 4.3 ) );
}

TEST_F(FloatSemiringTest, testMultiplication)
{
	EXPECT_TRUE( (*first) * (*second) == FloatSemiring( 1.2 * 4.3 ) );
}

TEST_F(FloatSemiringTest, testStar)
{
	EXPECT_TRUE( (*null).star() == *one );
	EXPECT_TRUE( FloatSemiring(0.5).star() == FloatSemiring(2.0) );
}
