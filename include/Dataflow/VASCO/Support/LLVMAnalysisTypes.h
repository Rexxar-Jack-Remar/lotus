#pragma once

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <map>

namespace vasco {
namespace llvmir {

enum class Sign {
  Top,
  Negative,
  Zero,
  Positive,
  Bottom,
};

inline Sign meetSigns(Sign LHS, Sign RHS) {
  if (LHS == RHS) {
    return LHS;
  }
  if (LHS == Sign::Top) {
    return RHS;
  }
  if (RHS == Sign::Top) {
    return LHS;
  }
  return Sign::Bottom;
}

inline Sign negateSign(Sign Value) {
  switch (Value) {
  case Sign::Negative:
    return Sign::Positive;
  case Sign::Positive:
    return Sign::Negative;
  case Sign::Zero:
    return Sign::Zero;
  case Sign::Top:
    return Sign::Top;
  case Sign::Bottom:
    return Sign::Bottom;
  }
  return Sign::Bottom;
}

inline Sign plusSigns(Sign LHS, Sign RHS) {
  if (LHS == Sign::Top) {
    return RHS;
  }
  if (RHS == Sign::Top) {
    return LHS;
  }
  if (LHS == Sign::Bottom || RHS == Sign::Bottom) {
    return Sign::Bottom;
  }
  if (LHS == Sign::Zero) {
    return RHS;
  }
  if (RHS == Sign::Zero) {
    return LHS;
  }
  if (LHS == RHS) {
    return LHS;
  }
  return Sign::Bottom;
}

inline Sign multiplySigns(Sign LHS, Sign RHS) {
  if (LHS == Sign::Top) {
    return RHS == Sign::Zero ? Sign::Zero : Sign::Top;
  }
  if (RHS == Sign::Top) {
    return LHS == Sign::Zero ? Sign::Zero : Sign::Top;
  }
  if (LHS == Sign::Bottom || RHS == Sign::Bottom) {
    return Sign::Bottom;
  }
  if (LHS == Sign::Zero || RHS == Sign::Zero) {
    return Sign::Zero;
  }
  return LHS == RHS ? Sign::Positive : Sign::Negative;
}

struct ValueKey {
  const llvm::Value *Value = nullptr;
  bool IsReturnValue = false;

  static ValueKey forValue(const llvm::Value *TrackedValue) {
    return ValueKey{TrackedValue, false};
  }

  static ValueKey returnValue() { return ValueKey{nullptr, true}; }

  bool operator<(const ValueKey &Other) const {
    if (IsReturnValue != Other.IsReturnValue) {
      return IsReturnValue < Other.IsReturnValue;
    }
    return Value < Other.Value;
  }

  bool operator==(const ValueKey &Other) const {
    return IsReturnValue == Other.IsReturnValue && Value == Other.Value;
  }
};

template <typename T> using FlowMap = std::map<ValueKey, T>;

inline const llvm::Value *stripCasts(const llvm::Value *Value) {
  auto *Current = Value;
  while (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Current)) {
    Current = Cast->getOperand(0);
  }
  return Current;
}

} // namespace llvmir
} // namespace vasco
