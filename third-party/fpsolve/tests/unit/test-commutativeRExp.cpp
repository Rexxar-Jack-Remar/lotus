#include "test-commutativeRExp.h"
#include "util.h"

void CommutativeRExpTest::SetUp()
{
	std::cout << "CRegExp-Test :" << std::endl;
	a = new CommutativeRExp(Var::GetVarId("a"));
	b = new CommutativeRExp(Var::GetVarId("b"));
	c = new CommutativeRExp(Var::GetVarId("c"));
}

void CommutativeRExpTest::TearDown()
{
	delete a;
	delete b;
	delete c;
}

TEST_F(CommutativeRExpTest, testSemiring)
{
  generic_test_semiring(*a,*b);
}

TEST_F(CommutativeRExpTest, testTerms)
{
/*	(a+(b.c+c.b).(a.b + c + b.a)*) = a + (b.c).(ab + c)*
	(a.b+c) + (c + b.a) = (a.b + c)
	(c + b.a) . (a.b + c) = (a.b+c).(a.b+c)
	(c + b.a) + (a.b + c) = (a.b + c)
	(a.b + a.c) + (a . (c+b)) = (a.b + a.c) +  (a . (b+c))*/

	CommutativeRExp a = *this->a;
	CommutativeRExp b = *this->b;
	CommutativeRExp c = *this->c;

	EXPECT_TRUE( ((a+(b*c+c*b)*(a*b + c + b*a).star())) == ( a + (b*c)*(a*b + c).star() ) );
	EXPECT_TRUE( ((a*b+c) + (c + b*a)) == ( (a*b + c) ) );
	EXPECT_TRUE( ((c + b*a) * (a*b + c)) == ( (a*b+c)*(a*b+c) ) );
	EXPECT_TRUE( ((c + b*a) + (a*b + c) ) == ( (a*b + c) ) );
	EXPECT_TRUE( ((a*b + a*c) + (a * (c+b)) ) == ( (a*b + a*c) +  (a * (b+c))) );
}

TEST_F(CommutativeRExpTest, testAddition)
{
	// a + 0 = a
	EXPECT_TRUE( (*a) + CommutativeRExp::null() == (*a) );
	// 0 + a = a
	EXPECT_TRUE( CommutativeRExp::null() + (*a) == (*a) );

	// associative (a + b) + c == a + (b + c)
	EXPECT_TRUE( ((*a) + (*b)) + (*c) == (*a) + ((*b) + (*c)) );
}

TEST_F(CommutativeRExpTest, testMultiplication)
{
	// a . 1 = a
	EXPECT_TRUE( (*a) * CommutativeRExp::one() == (*a) );
	// 1 . a = a
	EXPECT_TRUE( CommutativeRExp::one() * (*a) == (*a) );
	// a . 0 = 0
	EXPECT_TRUE( (*a) * CommutativeRExp::null() == CommutativeRExp::null() );
	// 0 . a = 0
	EXPECT_TRUE( CommutativeRExp::null() * (*a) == CommutativeRExp::null() );

	// associative (a * b) * c == a * (b * c)
	EXPECT_TRUE( ((*a) * (*b)) * (*c) == (*a) * ((*b) * (*c)) );
}

TEST_F(CommutativeRExpTest, testStar)
{
	// 0* = 1
	EXPECT_TRUE( CommutativeRExp::null().star() == CommutativeRExp::one() );

	EXPECT_TRUE( a->star() == CommutativeRExp(CommutativeRExp::Star, std::shared_ptr<CommutativeRExp>(new CommutativeRExp(*a))));

  // a.b^* != a.(a+b)^*
  EXPECT_TRUE( (*a) * (*b).star() !=  (*a) * ((*a) + (*b)).star());

}
