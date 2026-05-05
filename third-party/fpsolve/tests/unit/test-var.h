#ifndef TEST_VAR_H
#define TEST_VAR_H

#include "fpsolve/datastructs/var.h"
#include <gtest/gtest.h>

class VarTest : public ::testing::Test
{

public:
	void SetUp() override;
	void TearDown() override;

protected:
	void testIdentity();
};

#endif
