#include "test-bool-semiring.h"
#include "util.h"

void BoolSemiringTest::SetUp()
{
  std::cout << "Bool-SR-Test :" << std::endl;
}

void BoolSemiringTest::TearDown()
{
}

TEST_F(BoolSemiringTest, testSemiring)
{
  generic_test_semiring(BoolSemiring::null(), BoolSemiring::one());
}

TEST_F(BoolSemiringTest, testStar)
{
  EXPECT_TRUE( BoolSemiring::null().star() == BoolSemiring::one() );
  EXPECT_TRUE( BoolSemiring::one().star() == BoolSemiring::one() );
}
