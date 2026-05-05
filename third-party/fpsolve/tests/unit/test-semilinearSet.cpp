/*
 * test-semilinearSet.cpp
 *
 *  Created on: 16.04.2014
 *      Author: schlund
 */

#include "test-semilinearSet.h"
#include "util.h"


void SemilinSetTest::SetUp()
{
  std::cout << "SemilinearSet-Test :" << std::endl;
  a = new SLSet(Var::GetVarId("a"));
  b = new SLSet(Var::GetVarId("b"));
  c = new SLSet(Var::GetVarId("c"));
  d = new SLSet(SLSet::one());
  e = new SLSet(SLSet::one());
}

void SemilinSetTest::TearDown()
{
  delete a;
  delete b;
  delete c;
  delete d;
  delete e;
}

TEST_F(SemilinSetTest, testSemiring)
{
  generic_test_semiring(*a, *b);
  generic_test_semiring(*e, *c);
}

TEST_F(SemilinSetTest, testBasic)
{
}

TEST_F(SemilinSetTest, testTerms)
{
/*  (a+(b.c+c.b).(a.b + c + b.a)*) = a + (b.c).(ab + c)*
  (a.b+c) + (c + b.a) = (a.b + c)
  (c + b.a) . (a.b + c) = (a.b+c).(a.b+c)
  (c + b.a) + (a.b + c) = (a.b + c)
  (a.b + a.c) + (a . (c+b)) = (a.b + a.c) +  (a . (b+c))*/

  SLSet a = *this->a;
  SLSet b = *this->b;
  SLSet c = *this->c;

  EXPECT_TRUE( ((a+(b*c+c*b)*(a*b + c + b*a).star())) == ( a + (b*c)*(a*b + c).star() ) );
  EXPECT_TRUE( ((a*b+c) + (c + b*a)) == ( (a*b + c) ) );
  EXPECT_TRUE( ((c + b*a) * (a*b + c)) == ( (a*b+c)*(a*b+c) ) );
  EXPECT_TRUE( ((c + b*a) + (a*b + c) ) == ( (a*b + c) ) );
  EXPECT_TRUE( ((a*b + a*c) + (a * (c+b)) ) == ( (a*b + a*c) +  (a * (b+c))) );

  //this test only makes sense if we have a semantic equivalence checker (-> genepi)
#ifdef USE_GENEPI
  SLSet s1 = ((SLSet("<a:1,b:2>") * SLSet("<a:1,b:1>").star() * SLSet("<b:1>").star()) +
              (SLSet("<a:2,b:1>") * SLSet("<a:1,b:1>").star() * SLSet("<a:1>").star()) +
              (SLSet("<a:2,b:2>") * SLSet("<a:1,b:1>").star()) );
  SLSet s2 = ((SLSet("<a:1,b:2>") * SLSet("<b:1>").star()) +
              (SLSet("<a:2,b:1>") * SLSet("<a:1>").star() * SLSet("<b:1>").star()) );

  //std::cout << s1 << std::endl;
  //std::cout << s2 << std::endl;
  EXPECT_TRUE( s1  == s2);

  //TODO: have more "expected negatives"
  EXPECT_TRUE( s1  != (s2 + SLSet("<b:1>")));
#endif

}

TEST_F(SemilinSetTest, testAddition)
{
  // a + 0 = a
  EXPECT_TRUE( (*a) + SLSet::null() == (*a) );
  // 0 + a = a
  EXPECT_TRUE( SLSet::null() + (*a) == (*a) );

  // associativity (a + b) + c == a + (b + c)
  EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(SemilinSetTest, testMultiplication)
{

  // a.a != a.a.a
  EXPECT_TRUE( (*a) * (*a) !=  pow((*a),3));
  // a.a.a != a.a.a.a
  EXPECT_TRUE( (*a) * (*a) * (*a) !=   (*a) * (*a) * (*a) * (*a));

  // a.a != a.a.a
  EXPECT_TRUE( (*a) * (*a) !=  pow((*a),3));

  // a . 1 = a
  EXPECT_TRUE( (*a) * SLSet::one() == (*a) );
  // 1 . a = a
  EXPECT_TRUE( SLSet::one() * (*a) == (*a) );
  // a . 0 = 0
  EXPECT_TRUE( (*a) * SLSet::null() == SLSet::null() );
  // 0 . a = 0
  EXPECT_TRUE( SLSet::null() * (*a) == SLSet::null() );

  // associativity (a * b) * c == a * (b * c)
  EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );

  // commutativity with a more "complicated" expression (a+b+c)* . (c+b) = (c+b) . (a+b+c)*
  EXPECT_TRUE( ((*a) + (*b) + *(c)).star() * ( (*c) + (*c) + (*b) + (*b) )== ( (*c) + (*c) + (*b) + (*b) ) * ((*a) + (*b) + *(c)).star() );

}

TEST_F(SemilinSetTest, testStar)
{
  // 0* = 1
  EXPECT_TRUE( SLSet::null().star() == SLSet::one() );

  //1* = 1
  EXPECT_TRUE( SLSet::one().star() == SLSet::one() );

  // a.(b+c)^* == a.b^*c^* (holds for semilinear sets and pseudolinear sets)
  EXPECT_TRUE( (*a) * (*b).star() * (*c).star() == (*a) * ((*b) + (*c)).star() );

  // a.b^* + a.c^* != a.((b+c)^*)
  EXPECT_TRUE( (*a) * (*b).star() + (*a) * (*c).star() !=  (*a) * (*b).star() * (*c).star());

  // a.b^* != a.(a+b)^*
  EXPECT_TRUE( (*a) * (*b).star() !=  (*a) * ((*a) + (*b)).star());


}


