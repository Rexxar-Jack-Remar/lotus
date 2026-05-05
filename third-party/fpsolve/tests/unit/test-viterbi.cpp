
/*
 * test-viterbi.cpp
 *
 *  Created on: 22.07.2014
 *      Author: schlund
 */

/*
 * test-why-set.cpp
 *
 *  Created on: 15.05.2014
 *      Author: schlund
 */


#include "test-viterbi.h"
#include "util.h"

void ViterbiSemiringTest::SetUp()
{
  std::cout << "Viterbi-SR-Test :" << std::endl;
  a = new ViterbiSemiring("0.1");
  b = new ViterbiSemiring("0.9");
  c = new ViterbiSemiring("0.05");
  d = new ViterbiSemiring("1.0");
}

void ViterbiSemiringTest::TearDown()
{
  delete a;
  delete b;
  delete c;
  delete d;
}

TEST_F(ViterbiSemiringTest, testSemiring)
{
  generic_test_semiring(*a, *b);
  generic_test_semiring(*a+*c, *b+*d);
}

TEST_F(ViterbiSemiringTest, testAddition)
{
  std::cout << "a+b=" <<*a + *b << std::endl;
  EXPECT_TRUE( (*a) + ViterbiSemiring::null() == (*a));
  EXPECT_TRUE( ViterbiSemiring::null() + (*a) == (*a));
}

TEST_F(ViterbiSemiringTest, testMultiplication)
{
  std::cout << "a*b=" << *a* *b << std::endl;
  std::cout << "(a+b)*(a+b)=" << ((*a + *b)*(*a + *b)) << std::endl;
  std::cout << "1+(a+b)=" << (ViterbiSemiring::one()+(*a + *b)) << std::endl;
  std::cout << "(a+b)^2=" << pow((*a + *b),2) << std::endl;
}

TEST_F(ViterbiSemiringTest, testStar)
{
  ViterbiSemiring r = *a + *b + *c;
  std::cout << "r^3=" << (ViterbiSemiring::one()*r*(r*r)) << std::endl;
  ViterbiSemiring ab = *a * *b;
  ViterbiSemiring ac = *a * *c;
  ViterbiSemiring bc = *b * *c;
  ViterbiSemiring abc = *a * *b * *c;
  EXPECT_TRUE( r.star() == ViterbiSemiring::one() + *a + *b + *c + ab + ac + bc + abc);
}



