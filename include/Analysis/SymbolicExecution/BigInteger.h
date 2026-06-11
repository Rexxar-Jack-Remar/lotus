/** @file BigInteger.h @brief Arbitrary-precision integer support for symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_BIGINTEGER_H
#define ANALYSIS_SYMBOLICEXECUTION_BIGINTEGER_H

#include "llvm/ADT/APInt.h"

#include "Analysis/SymbolicExecution/PropertyValue.h"

using namespace llvm;

namespace SymbolicExecution {
class BigInteger {
public:
  APInt Val;
  // constant created by the analyzer.
  BigInteger(int64_t V) {
    APInt Tmp(sizeof(int64_t) * 8, V, true);
    // use minimum bitwidth to store a constant
    Val = APInt(Tmp.getMinSignedBits(), V, true);
  }
  BigInteger(APInt V) {
    if (V.getBitWidth() == 1) {
      Val = APInt(8, V.getZExtValue(),
                  true); // we interpret true as 1 instead of -1
    } else {
      Val = std::move(V);
    }
  }
  BigInteger() : BigInteger(0) {}

  BigInteger operator+(const BigInteger &R) const;
  BigInteger operator-(const BigInteger &R) const;
  BigInteger operator-() const;
  BigInteger operator*(const BigInteger &R) const;
  BigInteger &operator+=(const BigInteger &R);
  BigInteger &operator-=(const BigInteger &R);
  BigInteger &operator*=(const BigInteger &R);
  BigInteger sdiv(const BigInteger &R) const;
  BigInteger udiv(const BigInteger &R) const;

  bool operator==(const BigInteger &R) const;
  bool operator<(const BigInteger &R) const;
  bool operator<=(const BigInteger &R) const;
  bool operator>(const BigInteger &R) const;
  bool operator>=(const BigInteger &R) const;
  bool operator!=(const BigInteger &R) const;

  APInt getVal() const { return Val; }
  BigInteger doBinOp(const BigInteger &R, PropertyValue::BinOp Op) const;

  int64_t getAsBoundInt() const {
    // FIXME
    if (Val.getMinSignedBits() > 64) {
      return 0;
    }

    return Val.getSExtValue();
  }

private:
  void normalizeWidth(APInt &L, APInt &R) const;
};
} // namespace SymbolicExecution

#endif
