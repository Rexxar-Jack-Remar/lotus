#include "Dataflow/NPA/Solver/TensorProductLift.h"

#include <gtest/gtest.h>

#include <string>

namespace {

struct TraceSemiring {
  using value_type = std::string;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return "0"; }
  static value_type one() { return "1"; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    return "(" + a + "+" + b + ")";
  }
  static value_type extend(const value_type &a, const value_type &b) {
    // a after b
    return "(" + a + "∘" + b + ")";
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return "(" + a + "-" + b + ")";
  }
};

} // namespace

TEST(NPA, TensorProductRightComponentUsesOppositeProduct) {
  using D = TraceSemiring;
  using TD = npa::TensorProductLift<D>;

  // extend(a, b) means "a after b"
  //
  // In the tensor product used by NPA, extend is defined as:
  // (l1,r1) ⊗_p (l2,r2) = (l2∘l1, r1∘r2)
  TD::value_type a{"L1", "R1"};
  TD::value_type b{"L2", "R2"};

  auto c = TD::extend(a, b);
  EXPECT_EQ(c.first, "(L2∘L1)");
  EXPECT_EQ(c.second, "(R1∘R2)");
}
