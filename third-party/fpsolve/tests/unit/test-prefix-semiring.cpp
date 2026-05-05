#include "test-prefix-semiring.h"
#include "util.h"

void PrefixSemiringTest::SetUp()
{
  std::cout << "Prefix-SR-Test :" << std::endl;
  auto a = Var::GetVarId("a");
  auto b = Var::GetVarId("b");
  auto c = Var::GetVarId("c");
  first = new PrefixSemiring({a,b,a,b},5);
  second = new PrefixSemiring({b,a,b,a},5);
}

void PrefixSemiringTest::TearDown()
{
  delete first;
  delete second;
}

TEST_F(PrefixSemiringTest, testSemiring)
{
  generic_test_semiring(*first, *second);
}

TEST_F(PrefixSemiringTest, testStar)
{
  EXPECT_TRUE( PrefixSemiring::null().star() == PrefixSemiring::one() );
  auto a = Var::GetVarId("a");
  auto b = Var::GetVarId("b");
  auto t = PrefixSemiring({a,b,a},10);
  auto s = PrefixSemiring::one();
  s += PrefixSemiring({a,b,a},10);
  s += PrefixSemiring({a,b,a,a,b,a},10);
  s += PrefixSemiring({a,b,a,a,b,a,a,b,a},10);
  s += PrefixSemiring({a,b,a,a,b,a,a,b,a,a},10);
  EXPECT_TRUE( t.star() == s );
}
