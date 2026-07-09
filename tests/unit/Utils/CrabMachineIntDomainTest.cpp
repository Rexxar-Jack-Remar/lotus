#include <crab/domains/swrapped_interval.hpp>
#include <crab/domains/tnum.hpp>
#include <crab/numbers/bignums.hpp>
#include <crab/numbers/wrapint.hpp>
#include <gtest/gtest.h>

namespace {

using crab::wrapint;
using crab::domains::swrapped_interval;
using crab::domains::tnum;
using ikos::z_number;

TEST(CrabMachineIntDomainTest, WrappedIntervalMembershipIncludesEndpoints) {
  swrapped_interval<z_number> zero(wrapint(0, 8));
  swrapped_interval<z_number> one(wrapint(1, 8));

  EXPECT_TRUE(zero.at(wrapint(0, 8)));
  EXPECT_TRUE(one.at(wrapint(1, 8)));
}

TEST(CrabMachineIntDomainTest, TnumSingletonRangeKeepsWidth) {
  tnum<z_number> range =
      tnum<z_number>::tnum_from_range(wrapint(5, 8), wrapint(5, 8));

  EXPECT_TRUE(range.at(wrapint(5, 8)));
  EXPECT_EQ(range.value().get_bitwidth(), 8u);
  EXPECT_EQ(range.mask().get_bitwidth(), 8u);
}

TEST(CrabMachineIntDomainTest, TnumSymbolicShiftEnumeratesValidAmounts) {
  tnum<z_number> value(wrapint(1, 8));
  tnum<z_number> shift =
      tnum<z_number>::tnum_from_range(wrapint(0, 8), wrapint(1, 8));

  tnum<z_number> shifted = value.Shl(shift);

  EXPECT_TRUE(shifted.at(wrapint(1, 8)));
  EXPECT_TRUE(shifted.at(wrapint(2, 8)));
  EXPECT_EQ(shifted.value().get_bitwidth(), 8u);
  EXPECT_EQ(shifted.mask().get_bitwidth(), 8u);
}

} // namespace
