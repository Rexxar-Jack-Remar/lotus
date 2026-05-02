#include "Verification/SymAbsAI/Domains/Affine.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <cassert>
#include <cstdint>
#include <cstring>

#include <z3++.h>

namespace symabs_ai {
namespace domains {
namespace {
uint64_t bitMask(unsigned bw) {
  assert(bw > 0 && bw <= 64);
  return bw == 64 ? ~uint64_t(0) : ((uint64_t(1) << bw) - 1);
}

uint64_t bitPattern(int64_t value) {
  uint64_t result;
  static_assert(sizeof(result) == sizeof(value), "unexpected int64_t size");
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

int64_t signedFromBitPattern(uint64_t value) {
  int64_t result;
  static_assert(sizeof(result) == sizeof(value), "unexpected int64_t size");
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

int64_t signedResidue(uint64_t value, unsigned bw) {
  value &= bitMask(bw);

  if (bw < 64) {
    uint64_t sign_bit = uint64_t(1) << (bw - 1);
    if (value & sign_bit)
      value |= ~bitMask(bw);
  }

  return signedFromBitPattern(value);
}

uint64_t magnitude(int64_t value) {
  uint64_t bits = bitPattern(value);
  return value < 0 ? uint64_t(0) - bits : bits;
}
} // namespace

bool Affine::joinWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Affine &>(av_other);

  if (isTop() || other.isTop()) {
    bool changed = !isTop();
    State_ = TOP;
    return changed;
  }

  if (other.isBottom())
    return false;

  if (isBottom()) {
    // other is not top or bottom here (would've been handled in previous
    // cases)
    State_ = VALUE;
    Delta_ = other.Delta_;
    return true;
  }

  // both `this' and `other' are proper values (not top or bottom)
  if (Delta_ == other.Delta_) {
    return false;
  } else {
    State_ = TOP;
    return true;
  }
}

bool Affine::meetWith(const AbstractValue &av_other) {
  auto other = dynamic_cast<const Affine &>(av_other);

  if (isBottom() || other.isBottom()) {
    bool changed = !isBottom();
    State_ = BOTTOM;
    return changed;
  }

  if (other.isTop())
    return false;

  if (isTop()) {
    // other is not top or bottom here (would've been handled in previous
    // cases)
    State_ = VALUE;
    Delta_ = other.Delta_;
    return true;
  }

  // both `this' and `other' are proper values (not top or bottom)
  if (Delta_ == other.Delta_) {
    return false;
  } else {
    State_ = BOTTOM;
    return true;
  }
}

bool Affine::updateWith(const ConcreteState &state) {
  uint64_t left = state[Left_];
  uint64_t right = state[Right_];
  unsigned bw = FunctionContext_.sortForType(Left_->getType()).bv_size();

  Affine aval(FunctionContext_, Left_, Right_);
  aval.State_ = VALUE;
  aval.Delta_ = signedResidue(left - right, bw);
  return joinWith(aval);
}

z3::expr Affine::toFormula(const ValueMapping &vmap, z3::context &zctx) const {
  if (isTop())
    return zctx.bool_val(true);

  if (isBottom())
    return zctx.bool_val(false);

  unsigned bw = FunctionContext_.sortForType(Left_->getType()).bv_size();
  z3::expr delta = zctx.bv_val(bitPattern(Delta_) & bitMask(bw), bw);

  return vmap[Left_] == vmap[Right_] + delta;
}

void Affine::havoc() { State_ = TOP; }

void Affine::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }

  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  out << Left_ << " = " << Right_;

  if (Delta_ != 0) {
    uint64_t abs_delta = magnitude(Delta_);
    if (Delta_ > 0)
      out << " + " << abs_delta;
    else
      out << " - " << abs_delta;
  }
}

bool Affine::isJoinableWith(const AbstractValue &other) const {
  if (const auto *other_val = dynamic_cast<const Affine *>(&other)) {
    if (other_val->Left_ == Left_ && other_val->Right_ == Right_) {
      return true;
    }
  }
  return false;
}

namespace {
DomainConstructor::Register
    _("Affine",
      "relational domain of affine equalities between pairs of variables",
      Affine::New);
} // namespace
} // namespace domains
} // namespace symabs_ai
