#include "test-var.h"

void VarTest::SetUp()
{
  std::cout << "Var-Test :" << std::endl;
}

void VarTest::TearDown()
{
}

TEST_F(VarTest, testIdentity)
{
	VarId a = Var::GetVarId("a");
	VarId b = Var::GetVarId("b");
	VarId newA = Var::GetVarId("a");
	
	EXPECT_TRUE( a == newA );
	EXPECT_TRUE( a != b );
}
