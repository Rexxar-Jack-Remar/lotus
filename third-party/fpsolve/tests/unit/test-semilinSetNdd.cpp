/*
 * test-semilinSetNdd.h
 *
 *  Created on: 28.01.2014
 *      Author: Michael Kerscher
 */

#include "test-semilinSetNdd.h"
#include "util.h"

void SemilinSetNddTest::SetUp()
{
	std::cout << "SL-NDD-Test :" << std::endl;
  SemilinSetNdd::solver_init(3);
	a = new SemilinSetNdd(Var::GetVarId("a"));
	b = new SemilinSetNdd(Var::GetVarId("b"));
	c = new SemilinSetNdd(Var::GetVarId("c"));
}

void SemilinSetNddTest::TearDown()
{
	delete a;
	delete b;
	delete c;
        // This triggers a bug, when we add something to null()
        // when setUp is called for the second time, null(), etc. should
        // be recreated to respect the change of the solver object
        // (at least that's what i think causes the bug)
	//SemilinSetNdd::solver_dealloc();
}

TEST_F(SemilinSetNddTest, testSemiring)
{
  generic_test_semiring(*a, *b);
}

TEST_F(SemilinSetNddTest, testBasic)
{
}

TEST_F(SemilinSetNddTest, testTerms)
{
/*	(a+(b.c+c.b).(a.b + c + b.a)*) = a + (b.c).(ab + c)*
	(a.b+c) + (c + b.a) = (a.b + c)
	(c + b.a) . (a.b + c) = (a.b+c).(a.b+c)
	(c + b.a) + (a.b + c) = (a.b + c)
	(a.b + a.c) + (a . (c+b)) = (a.b + a.c) +  (a . (b+c))*/

	SemilinSetNdd a = *this->a;
	SemilinSetNdd b = *this->b;
	SemilinSetNdd c = *this->c;

	EXPECT_TRUE( ((a+(b*c+c*b)*(a*b + c + b*a).star())) == ( a + (b*c)*(a*b + c).star() ) );
	EXPECT_TRUE( ((a*b+c) + (c + b*a)) == ( (a*b + c) ) );
	EXPECT_TRUE( ((c + b*a) * (a*b + c)) == ( (a*b+c)*(a*b+c) ) );
	EXPECT_TRUE( ((c + b*a) + (a*b + c) ) == ( (a*b + c) ) );
	EXPECT_TRUE( ((a*b + a*c) + (a * (c+b)) ) == ( (a*b + a*c) +  (a * (b+c))) );

	EXPECT_TRUE( (a.star() + b.star()).star() == a.star() * b.star());

	// very demanding testcase without optimizations
	EXPECT_TRUE( ((a.star() + b*b.star())*(a*b*c.star())).star()  ==
	    (SemilinSetNdd::one() + a*b*(a*b).star() * a.star() * c.star())* (SemilinSetNdd::one() + (a*b*b)*(a*b*b).star()*b.star()*c.star()));

  // (a+b)^* = a^*.b^* != a^* + b^*
  EXPECT_TRUE( (a+b).star() != a.star() + b.star());

  // for the wrong offset-minimization this does not terminate (the outer star computation loops indefinitely)
  EXPECT_TRUE( ((a+b.star())*(b+c.star())).star() ==
                  (SemilinSetNdd::one() + (a*b).star()) * (SemilinSetNdd::one() + b * b.star()) *
                  (SemilinSetNdd::one() + a * a.star()*c.star()) * (SemilinSetNdd::one() + b.star() * c.star()));

}

TEST_F(SemilinSetNddTest, testAddition)
{
	// a + 0 = a
	EXPECT_TRUE( (*a) + SemilinSetNdd::null() == (*a) );
	// 0 + a = a
	EXPECT_TRUE( SemilinSetNdd::null() + (*a) == (*a) );

	// associativity (a + b) + c == a + (b + c)
	EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(SemilinSetNddTest, testMultiplication)
{

  // a.a != a.a.a
  EXPECT_TRUE( (*a) * (*a) !=  pow((*a),3));
  // a.a.a != a.a.a.a
  EXPECT_TRUE( (*a) * (*a) * (*a) !=   (*a) * (*a) * (*a) * (*a));


	// a . 1 = a
	EXPECT_TRUE( (*a) * SemilinSetNdd::one() == (*a) );
	// 1 . a = a
	EXPECT_TRUE( SemilinSetNdd::one() * (*a) == (*a) );
	// a . 0 = 0
	EXPECT_TRUE( (*a) * SemilinSetNdd::null() == SemilinSetNdd::null() );
	// 0 . a = 0
	EXPECT_TRUE( SemilinSetNdd::null() * (*a) == SemilinSetNdd::null() );

	// associativity (a * b) * c == a * (b * c)
	EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );

	// commutativity with a more "complicated" expression (a+b+c)* . (c+b) = (c+b) . (a+b+c)*
	EXPECT_TRUE( ((*a) + (*b) + *(c)).star() * ( (*c) + (*c) + (*b) + (*b) )== ( (*c) + (*c) + (*b) + (*b) ) * ((*a) + (*b) + *(c)).star() );
}

TEST_F(SemilinSetNddTest, testStar)
{
	// 0* = 1
	EXPECT_TRUE( SemilinSetNdd::null().star() == SemilinSetNdd::one() );

	//1* = 1
	EXPECT_TRUE( SemilinSetNdd::one().star() == SemilinSetNdd::one() );

  // a.b^* != a.(a+b)^*
  EXPECT_TRUE( (*a) * (*b).star() !=  (*a) * ((*a) + (*b)).star());

}
