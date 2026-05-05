/*
 * test-pseudolinearSet.h
 *
 *  Created on: 20.12.2014
 *      Author: schlund
 */

#pragma once

#include <gtest/gtest.h>

#include "fpsolve/semirings/pseudo_linear_set.h"


typedef PseudoLinearSet<> PLSet;

class PseudolinSetTest : public ::testing::Test
{

public:
  void SetUp() override;
  void TearDown() override;

protected:
  void testSemiring();
  void testBasic();
  void testAddition();
  void testMultiplication();
  void testStar();
  void testTerms();

protected:
  PLSet *a, *b, *c, *d, *e;
};
