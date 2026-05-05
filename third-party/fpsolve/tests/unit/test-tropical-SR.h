/*
 * test-tropical-SR.h
 *
 *  Created on: 14.03.2013
 *      Author: schlund
 */

#pragma once

#include <gtest/gtest.h>

#include "fpsolve/semirings/tropical-semiring.h"


class TropicalSemiringTest : public ::testing::Test
{

public:
  void SetUp() override;
  void TearDown() override;

protected:
  void testSemiring();
  void testAddition();
  void testMultiplication();
  void testStar();

protected:
  TropicalSemiring *first, *second;
};
