/**
 * @file FloatingPointModel.h
 * @brief Factory and base class for floating-point SMT modeling in SymAbsAI.
 *
 * Defines the FloatingPointModel class hierarchy that creates Z3 floating-point
 * sorts and operations for LLVM IR floating-point instructions. Supports
 * multiple floating-point semantics via the static New() factory method.
 */
#pragma once

#include "Verification/SymAbsAI/Core/Integration/FunctionContext.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <z3++.h>

namespace llvm {
class Type;
class ConstantFP;
} // namespace llvm

namespace symabs_ai {
/** @brief Abstract base for floating-point SMT modeling of LLVM IR. */
class FloatingPointModel {
  mutable unsigned RoundingModeCounter_;
  static constexpr const char *RM_PREFIX = "__ROUNDING_MODE_";

protected:
  const FunctionContext &FunctionContext_;
  z3::context &Ctx_;

  FloatingPointModel(const FunctionContext &fctx)
      : RoundingModeCounter_(0), FunctionContext_(fctx), Ctx_(fctx.getZ3()) {}

  z3::expr roundingMode() const;

public:
  virtual ~FloatingPointModel() = default;

  static unique_ptr<FloatingPointModel> New(const FunctionContext &fctx);

  virtual bool supportsType(llvm::Type *type) const;
  virtual z3::sort sortForType(llvm::Type *type) const;
  virtual z3::expr literal(llvm::ConstantFP *constant) const;

  virtual z3::expr transferBinop(llvm::BinaryOperator &binop, z3::expr res,
                                 z3::expr in0, z3::expr in1) const {
    return Ctx_.bool_val(true);
  }

  virtual z3::expr transferCmp(llvm::CmpInst::Predicate pred, z3::expr res,
                               z3::expr in0, z3::expr in1) const {
    return Ctx_.bool_val(true);
  }

  virtual z3::expr transferConversion(z3::expr res, llvm::Instruction *inst,
                                      z3::expr in) const {
    return Ctx_.bool_val(true);
  }
};
} // namespace symabs_ai
