/*
 * test-pseudolinearSet.cpp
 *
 *  Created on: 20.12.2014
 *      Author: schlund
 */

#include "test-pseudolinearSet.h"
#include "util.h"


void PseudolinSetTest::SetUp()
{
  std::cout << "Pseudo-linearSet-Test :" << std::endl;
  a = new PLSet(Var::GetVarId("a"));
  b = new PLSet(Var::GetVarId("b"));
  c = new PLSet(Var::GetVarId("c"));
  d = new PLSet(PLSet::one());
  e = new PLSet(PLSet::one());
}

void PseudolinSetTest::TearDown()
{
  delete a;
  delete b;
  delete c;
  delete d;
  delete e;
}

TEST_F(PseudolinSetTest, testSemiring)
{
  generic_test_semiring(*a, *b);
  generic_test_semiring(*e, *c);
}

TEST_F(PseudolinSetTest, testBasic)
{
}

TEST_F(PseudolinSetTest, testTerms)
{
/*  (a+(b.c+c.b).(a.b + c + b.a)*) = a + (b.c).(ab + c)*
  (a.b+c) + (c + b.a) = (a.b + c)
  (c + b.a) . (a.b + c) = (a.b+c).(a.b+c)
  (c + b.a) + (a.b + c) = (a.b + c)
  (a.b + a.c) + (a . (c+b)) = (a.b + a.c) +  (a . (b+c))*/

  PLSet a = *this->a;
  PLSet b = *this->b;
  PLSet c = *this->c;

  EXPECT_TRUE( ((a+(b*c+c*b)*(a*b + c + b*a).star())) == ( a + (b*c)*(a*b + c).star() ) );
  EXPECT_TRUE( ((a*b+c) + (c + b*a)) == ( (a*b + c) ) );
  EXPECT_TRUE( ((c + b*a) * (a*b + c)) == ( (a*b+c)*(a*b+c) ) );
  EXPECT_TRUE( ((c + b*a) + (a*b + c) ) == ( (a*b + c) ) );
  EXPECT_TRUE( ((a*b + a*c) + (a * (c+b)) ) == ( (a*b + a*c) +  (a * (b+c))) );
}

TEST_F(PseudolinSetTest, testAddition)
{
  // a + 0 = a
  EXPECT_TRUE( (*a) + PLSet::null() == (*a) );
  // 0 + a = a
  EXPECT_TRUE( PLSet::null() + (*a) == (*a) );

  // associativity (a + b) + c == a + (b + c)
  EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(PseudolinSetTest, testMultiplication)
{

  // a.a != a.a.a
  EXPECT_TRUE( (*a) * (*a) !=  pow((*a),3));
  // a.a.a != a.a.a.a
  EXPECT_TRUE( (*a) * (*a) * (*a) !=   (*a) * (*a) * (*a) * (*a));

  // a.a != a.a.a
  EXPECT_TRUE( (*a) * (*a) !=  pow((*a),3));

  // a . 1 = a
  EXPECT_TRUE( (*a) * PLSet::one() == (*a) );
  // 1 . a = a
  EXPECT_TRUE( PLSet::one() * (*a) == (*a) );
  // a . 0 = 0
  EXPECT_TRUE( (*a) * PLSet::null() == PLSet::null() );
  // 0 . a = 0
  EXPECT_TRUE( PLSet::null() * (*a) == PLSet::null() );

  // associativity (a * b) * c == a * (b * c)
  EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );

  // commutativity with a more "complicated" expression (a+b+c)* . (c+b) = (c+b) . (a+b+c)*
  EXPECT_TRUE( ((*a) + (*b) + *(c)).star() * ( (*c) + (*c) + (*b) + (*b) )== ( (*c) + (*c) + (*b) + (*b) ) * ((*a) + (*b) + *(c)).star() );
}

TEST_F(PseudolinSetTest, testStar)
{
  // 0* = 1
  EXPECT_TRUE( PLSet::null().star() == PLSet::one() );

  //1* = 1
  EXPECT_TRUE( PLSet::one().star() == PLSet::one() );

  // a.b^* != a.(a+b)^*
  EXPECT_TRUE( (*a) * (*b).star() !=  (*a) * ((*a) + (*b)).star());

  // a.(b+c)^* == a.b^*c^* (holds for semilinear sets and pseudolinear sets)
  EXPECT_TRUE( (*a) * (*b).star() * (*c).star() == (*a) * ((*b) + (*c)).star() );

  // a.b^* + a.c^* == a.((b+c)^*) this holds only for pseudolinear sets!
  EXPECT_TRUE( (*a) * (*b).star() + (*a) * (*c).star() ==  (*a) * (*b).star() * (*c).star());


}



