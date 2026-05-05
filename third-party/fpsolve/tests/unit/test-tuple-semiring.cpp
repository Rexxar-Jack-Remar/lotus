#include "test-tuple-semiring.h"
#include "util.h"

void TupleSemiringTest::SetUp()
{
  std::cout << "Tuple-SR-Test :" << std::endl;
  first = new TupleSemiring<FloatSemiring,BoolSemiring>(FloatSemiring(1.2), BoolSemiring(false));
  second = new TupleSemiring<FloatSemiring,BoolSemiring>(FloatSemiring(0.5), BoolSemiring(true));
}

void TupleSemiringTest::TearDown()
{
  delete first;
  delete second;
}

TEST_F(TupleSemiringTest, testSemiring)
{
  generic_test_semiring(*first, *second);
}

TEST_F(TupleSemiringTest, testStar)
{
  auto null_star = TupleSemiring<FloatSemiring,BoolSemiring>::null().star();
  auto one = TupleSemiring<FloatSemiring,BoolSemiring>::one();
  EXPECT_TRUE( null_star == one );
  auto tmp = TupleSemiring<FloatSemiring,BoolSemiring>(FloatSemiring(2.0), BoolSemiring(true));
  EXPECT_TRUE( second->star() == tmp );
}
