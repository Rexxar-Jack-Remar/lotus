#include "Concurrency/CUDA/CUDASymbolicModel.h"

#include <algorithm>

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace concurrency::cuda {

namespace {

static std::optional<int64_t> addIfKnown(const std::optional<int64_t> &lhs,
                                         const std::optional<int64_t> &rhs,
                                         bool subtract = false) {
  if (!lhs || !rhs) {
    return std::nullopt;
  }
  int64_t result = 0;
  const bool overflow = subtract ? __builtin_sub_overflow(*lhs, *rhs, &result)
                                 : __builtin_add_overflow(*lhs, *rhs, &result);
  return overflow ? std::nullopt : std::optional<int64_t>(result);
}

static bool combineCoefficient(int64_t lhs, int64_t rhs, bool subtract,
                               int64_t &result) {
  return !(subtract ? __builtin_sub_overflow(lhs, rhs, &result)
                    : __builtin_add_overflow(lhs, rhs, &result));
}

static bool scaleCoefficient(int64_t value, int64_t scale, int64_t &result) {
  return !__builtin_mul_overflow(value, scale, &result);
}

static BuiltinKind classifyName(StringRef name) {
  if (name.contains("nctaid.x") || name.contains("gridDim.x")) {
    return BuiltinKind::GridDimX;
  }
  if (name.contains("nctaid.y") || name.contains("gridDim.y")) {
    return BuiltinKind::GridDimY;
  }
  if (name.contains("nctaid.z") || name.contains("gridDim.z")) {
    return BuiltinKind::GridDimZ;
  }
  if (name.contains("ntid.x") || name.contains("blockDim.x")) {
    return BuiltinKind::BlockDimX;
  }
  if (name.contains("ntid.y") || name.contains("blockDim.y")) {
    return BuiltinKind::BlockDimY;
  }
  if (name.contains("ntid.z") || name.contains("blockDim.z")) {
    return BuiltinKind::BlockDimZ;
  }
  if (name.contains("tid.x") || name.contains("threadIdx.x")) {
    return BuiltinKind::ThreadIdxX;
  }
  if (name.contains("tid.y") || name.contains("threadIdx.y")) {
    return BuiltinKind::ThreadIdxY;
  }
  if (name.contains("tid.z") || name.contains("threadIdx.z")) {
    return BuiltinKind::ThreadIdxZ;
  }
  if (name.contains("ctaid.x") || name.contains("blockIdx.x")) {
    return BuiltinKind::BlockIdxX;
  }
  if (name.contains("ctaid.y") || name.contains("blockIdx.y")) {
    return BuiltinKind::BlockIdxY;
  }
  if (name.contains("ctaid.z") || name.contains("blockIdx.z")) {
    return BuiltinKind::BlockIdxZ;
  }
  if (name.contains("laneid")) {
    return BuiltinKind::LaneId;
  }
  if (name.contains("shuffle.down") || name.contains("nvvm.shfl.down")) {
    return BuiltinKind::ShuffleDown;
  }
  if (name.contains("shuffle.up") || name.contains("nvvm.shfl.up")) {
    return BuiltinKind::ShuffleUp;
  }
  if (name.contains("shuffle.xor") || name.contains("nvvm.shfl.xor")) {
    return BuiltinKind::ShuffleXor;
  }
  if (name.contains("shuffle") || name.contains("nvvm.shfl")) {
    return BuiltinKind::Shuffle;
  }
  if (name.contains("vote") || name.contains("nvvm.vote")) {
    if (name.contains("any")) {
      return BuiltinKind::VoteAny;
    }
    if (name.contains("all")) {
      return BuiltinKind::VoteAll;
    }
    return BuiltinKind::VoteBallot;
  }
  if (name.contains("lanemask") || name.contains("nvvm.lanemask")) {
    if (name.contains("lt")) {
      return BuiltinKind::LaneMaskLt;
    }
    if (name.contains("le")) {
      return BuiltinKind::LaneMaskLe;
    }
    if (name.contains("gt")) {
      return BuiltinKind::LaneMaskGt;
    }
    if (name.contains("ge")) {
      return BuiltinKind::LaneMaskGe;
    }
    return BuiltinKind::LaneMaskLt;
  }
  if (name.contains("warpsize") || name.contains("nvvm.warpsize")) {
    return BuiltinKind::WarpSize;
  }
  return BuiltinKind::None;
}

static bool dependsOnBuiltins(const Value *value,
                              std::initializer_list<BuiltinKind> kinds) {
  if (!value) {
    return false;
  }

  SmallVector<const Value *, 8> worklist;
  SmallPtrSet<const Value *, 16> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.pop_back_val();
    if (!visited.insert(current).second) {
      continue;
    }

    const BuiltinKind builtin = CUDASymbolicModel::classifyBuiltin(current);
    if (llvm::is_contained(kinds, builtin)) {
      return true;
    }

    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (const Value *operand : ce->operands()) {
        worklist.push_back(operand);
      }
    }
  }

  return false;
}

static void mergePatterns(AffineAccessPattern &dst,
                          const AffineAccessPattern &lhs,
                          const AffineAccessPattern &rhs,
                          bool subtract = false) {
  if (!lhs.valid || !rhs.valid) {
    dst.valid = false;
    dst.non_affine = lhs.non_affine || rhs.non_affine;
    return;
  }
  if (!combineCoefficient(lhs.constant, rhs.constant, subtract, dst.constant) ||
      !combineCoefficient(lhs.thread_idx_x, rhs.thread_idx_x, subtract,
                          dst.thread_idx_x) ||
      !combineCoefficient(lhs.thread_idx_y, rhs.thread_idx_y, subtract,
                          dst.thread_idx_y) ||
      !combineCoefficient(lhs.thread_idx_z, rhs.thread_idx_z, subtract,
                          dst.thread_idx_z) ||
      !combineCoefficient(lhs.block_idx_x, rhs.block_idx_x, subtract,
                          dst.block_idx_x) ||
      !combineCoefficient(lhs.block_idx_y, rhs.block_idx_y, subtract,
                          dst.block_idx_y) ||
      !combineCoefficient(lhs.block_idx_z, rhs.block_idx_z, subtract,
                          dst.block_idx_z) ||
      !combineCoefficient(lhs.lane_id, rhs.lane_id, subtract, dst.lane_id)) {
    dst = {};
    dst.non_affine = true;
    return;
  }
  dst.valid = true;
  dst.exact = lhs.exact && rhs.exact;
  dst.non_affine = lhs.non_affine || rhs.non_affine;
  dst.participation = std::max(lhs.participation, rhs.participation);
}

static bool scalePattern(AffineAccessPattern &pattern, int64_t scale) {
  AffineAccessPattern scaled = pattern;
  if (!scaleCoefficient(pattern.constant, scale, scaled.constant) ||
      !scaleCoefficient(pattern.thread_idx_x, scale, scaled.thread_idx_x) ||
      !scaleCoefficient(pattern.thread_idx_y, scale, scaled.thread_idx_y) ||
      !scaleCoefficient(pattern.thread_idx_z, scale, scaled.thread_idx_z) ||
      !scaleCoefficient(pattern.block_idx_x, scale, scaled.block_idx_x) ||
      !scaleCoefficient(pattern.block_idx_y, scale, scaled.block_idx_y) ||
      !scaleCoefficient(pattern.block_idx_z, scale, scaled.block_idx_z) ||
      !scaleCoefficient(pattern.lane_id, scale, scaled.lane_id)) {
    return false;
  }
  pattern = scaled;
  return true;
}

static void markNonAffine(AffineAccessPattern &pattern) {
  pattern = {};
  pattern.non_affine = true;
}

static AffineAccessPattern makeConstantPattern(int64_t constant) {
  AffineAccessPattern pattern;
  pattern.constant = constant;
  pattern.valid = true;
  pattern.exact = true;
  return pattern;
}

static AffineAccessPattern normalizePattern(AffineAccessPattern pattern) {
  if (!pattern.valid) {
    return pattern;
  }
  if (pattern.non_affine) {
    pattern.exact = false;
  }
  return pattern;
}

static AffineAccessPattern
mergeNormalizedPatterns(const AffineAccessPattern &lhs,
                        const AffineAccessPattern &rhs, bool subtract = false) {
  AffineAccessPattern merged;
  mergePatterns(merged, lhs, rhs, subtract);
  return normalizePattern(merged);
}

static bool tryScalePattern(AffineAccessPattern &pattern, int64_t scale) {
  if (!pattern.valid) {
    return false;
  }
  if (!scalePattern(pattern, scale)) {
    markNonAffine(pattern);
    return false;
  }
  pattern = normalizePattern(pattern);
  return true;
}

static bool tryDividePattern(AffineAccessPattern &pattern, int64_t divisor) {
  if (!pattern.valid || divisor == 0 || !pattern.isDivisibleBy(divisor)) {
    return false;
  }
  pattern.divideBy(divisor);
  pattern = normalizePattern(pattern);
  return true;
}

static const Module *getEnclosingModule(const Value *value) {
  if (const auto *inst = dyn_cast_or_null<Instruction>(value)) {
    return inst->getModule();
  }
  if (const auto *argument = dyn_cast_or_null<Argument>(value)) {
    return argument->getParent() ? argument->getParent()->getParent() : nullptr;
  }
  if (const auto *global = dyn_cast_or_null<GlobalValue>(value)) {
    return global->getParent();
  }
  if (const auto *op = dyn_cast_or_null<Operator>(value)) {
    for (const Value *operand : op->operands()) {
      if (const Module *module = getEnclosingModule(operand)) {
        return module;
      }
    }
  }
  return nullptr;
}

static int64_t normalizeDimension(int64_t dim) { return dim > 0 ? dim : 1; }

static CanonicalAffineAccessPattern
canonicalizePattern(const AffineAccessPattern &pattern,
                    const std::array<int64_t, 3> &block_dims,
                    const std::array<int64_t, 3> &grid_dims) {
  CanonicalAffineAccessPattern canonical;
  if (!pattern.valid) {
    return canonical;
  }

  const int64_t block_x = normalizeDimension(block_dims[0]);
  const int64_t block_y = normalizeDimension(block_dims[1]);
  const int64_t grid_x = normalizeDimension(grid_dims[0]);
  const int64_t grid_y = normalizeDimension(grid_dims[1]);

  canonical.constant = pattern.constant;
  canonical.linear_thread = pattern.thread_idx_x +
                            pattern.thread_idx_y * block_x +
                            pattern.thread_idx_z * block_x * block_y;
  canonical.linear_block = pattern.block_idx_x + pattern.block_idx_y * grid_x +
                           pattern.block_idx_z * grid_x * grid_y;
  canonical.lane = pattern.lane_id;
  canonical.thread_stride_bytes = canonical.linear_thread;
  canonical.block_stride_bytes = canonical.linear_block;
  canonical.valid = true;
  canonical.exact = pattern.exact && !pattern.non_affine;
  return canonical;
}

static UniformityClass mergeUniformity(UniformityClass lhs,
                                       UniformityClass rhs) {
  if (lhs == UniformityClass::ThreadVarying ||
      rhs == UniformityClass::ThreadVarying) {
    return UniformityClass::ThreadVarying;
  }
  if (lhs == UniformityClass::Unknown || rhs == UniformityClass::Unknown) {
    return UniformityClass::Unknown;
  }
  if (lhs == UniformityClass::WarpUniform ||
      rhs == UniformityClass::WarpUniform) {
    return UniformityClass::WarpUniform;
  }
  return UniformityClass::BlockUniform;
}

} // namespace

BuiltinKind CUDASymbolicModel::classifyBuiltin(const Value *value) {
  if (!value) {
    return BuiltinKind::None;
  }
  if (const auto *call = dyn_cast<CallBase>(value)) {
    if (const Function *callee = call->getCalledFunction()) {
      return classifyName(callee->getName());
    }
  }
  if (const auto *gv = dyn_cast<GlobalValue>(value)) {
    return classifyName(gv->getName());
  }
  if (const auto *inst = dyn_cast<Instruction>(value);
      inst && inst->hasName()) {
    return classifyName(inst->getName());
  }
  if (const auto *arg = dyn_cast<Argument>(value); arg && arg->hasName()) {
    return classifyName(arg->getName());
  }
  return BuiltinKind::None;
}

bool CUDASymbolicModel::dependsOnThreadBuiltin(const Value *value) {
  return dependsOnBuiltins(value,
                           {BuiltinKind::ThreadIdxX, BuiltinKind::ThreadIdxY,
                            BuiltinKind::ThreadIdxZ, BuiltinKind::LaneId,
                            BuiltinKind::Shuffle, BuiltinKind::ShuffleDown,
                            BuiltinKind::ShuffleUp, BuiltinKind::ShuffleXor,
                            BuiltinKind::LaneMaskLt, BuiltinKind::LaneMaskLe,
                            BuiltinKind::LaneMaskGt, BuiltinKind::LaneMaskGe});
}

bool CUDASymbolicModel::dependsOnBlockBuiltin(const Value *value) {
  return dependsOnBuiltins(
      value,
      {BuiltinKind::BlockIdxX, BuiltinKind::BlockIdxY, BuiltinKind::BlockIdxZ});
}

bool CUDASymbolicModel::dependsOnLaneBuiltin(const Value *value) {
  return dependsOnBuiltins(value, {BuiltinKind::LaneId});
}

std::optional<int64_t>
CUDASymbolicModel::evaluateConstantInt(const Value *value) {
  if (!value) {
    return std::nullopt;
  }
  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    return ci->getSExtValue();
  }
  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->getOpcode() == Instruction::Add) {
      return addIfKnown(evaluateConstantInt(ce->getOperand(0)),
                        evaluateConstantInt(ce->getOperand(1)));
    }
    if (ce->getOpcode() == Instruction::Sub) {
      return addIfKnown(evaluateConstantInt(ce->getOperand(0)),
                        evaluateConstantInt(ce->getOperand(1)), true);
    }
    if (ce->getOpcode() == Instruction::Mul) {
      auto lhs = evaluateConstantInt(ce->getOperand(0));
      auto rhs = evaluateConstantInt(ce->getOperand(1));
      if (lhs && rhs) {
        int64_t result = 0;
        return __builtin_mul_overflow(*lhs, *rhs, &result)
                   ? std::nullopt
                   : std::optional<int64_t>(result);
      }
    }
    return std::nullopt;
  }
  if (const auto *inst = dyn_cast<Instruction>(value)) {
    if (inst->getOpcode() == Instruction::Add) {
      return addIfKnown(evaluateConstantInt(inst->getOperand(0)),
                        evaluateConstantInt(inst->getOperand(1)));
    }
    if (inst->getOpcode() == Instruction::Sub) {
      return addIfKnown(evaluateConstantInt(inst->getOperand(0)),
                        evaluateConstantInt(inst->getOperand(1)), true);
    }
    if (inst->getOpcode() == Instruction::Mul ||
        inst->getOpcode() == Instruction::Shl) {
      auto lhs = evaluateConstantInt(inst->getOperand(0));
      auto rhs = evaluateConstantInt(inst->getOperand(1));
      if (lhs && rhs) {
        if (inst->getOpcode() == Instruction::Mul) {
          int64_t result = 0;
          return __builtin_mul_overflow(*lhs, *rhs, &result)
                     ? std::nullopt
                     : std::optional<int64_t>(result);
        }
        const auto *type = dyn_cast<IntegerType>(inst->getType());
        if (!type || *rhs < 0 ||
            static_cast<uint64_t>(*rhs) >= type->getBitWidth()) {
          return std::nullopt;
        }
        APInt value(type->getBitWidth(), static_cast<uint64_t>(*lhs), true);
        value <<= static_cast<unsigned>(*rhs);
        return value.isSignedIntN(64)
                   ? std::optional<int64_t>(value.getSExtValue())
                   : std::nullopt;
      }
    }
    if (inst->getOpcode() == Instruction::And) {
      auto lhs = evaluateConstantInt(inst->getOperand(0));
      auto rhs = evaluateConstantInt(inst->getOperand(1));
      if (lhs && rhs) {
        return (*lhs) & (*rhs);
      }
    }
  }
  return std::nullopt;
}

AffineAccessPattern
CUDASymbolicModel::extractAffineAccessPattern(const Value *value) {
  AffineAccessPattern pattern;
  if (!value) {
    return pattern;
  }

  switch (classifyBuiltin(value)) {
  case BuiltinKind::ThreadIdxX:
    pattern.thread_idx_x = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Block;
    return pattern;
  case BuiltinKind::ThreadIdxY:
    pattern.thread_idx_y = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Block;
    return pattern;
  case BuiltinKind::ThreadIdxZ:
    pattern.thread_idx_z = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Block;
    return pattern;
  case BuiltinKind::BlockIdxX:
    pattern.block_idx_x = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Grid;
    return pattern;
  case BuiltinKind::BlockIdxY:
    pattern.block_idx_y = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Grid;
    return pattern;
  case BuiltinKind::BlockIdxZ:
    pattern.block_idx_z = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Grid;
    return pattern;
  case BuiltinKind::LaneId:
    pattern.lane_id = 1;
    pattern.valid = true;
    pattern.exact = true;
    pattern.participation = ParticipationScope::Warp;
    return pattern;
  default:
    break;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    return makeConstantPattern(ci->getSExtValue());
  }

  if (const auto *gep = dyn_cast<GEPOperator>(value)) {
    if (gep->getNumIndices() == 0) {
      return extractAffineAccessPattern(gep->getPointerOperand());
    }
    const Module *module = getEnclosingModule(value);
    if (!module) {
      markNonAffine(pattern);
      return pattern;
    }
    const DataLayout &layout = module->getDataLayout();
    const unsigned bit_width =
        layout.getIndexSizeInBits(gep->getPointerAddressSpace());
    MapVector<Value *, APInt> variable_offsets;
    APInt constant_offset(bit_width, 0, true);
    if (!gep->collectOffset(layout, bit_width, variable_offsets,
                            constant_offset) ||
        !constant_offset.isSignedIntN(64)) {
      markNonAffine(pattern);
      return pattern;
    }
    pattern = makeConstantPattern(constant_offset.getSExtValue());
    for (const auto &entry : variable_offsets) {
      if (!entry.second.isSignedIntN(64)) {
        markNonAffine(pattern);
        return pattern;
      }
      AffineAccessPattern index_pattern =
          extractAffineAccessPattern(entry.first);
      if (!index_pattern.valid ||
          !tryScalePattern(index_pattern, entry.second.getSExtValue())) {
        markNonAffine(pattern);
        return pattern;
      }
      pattern = mergeNormalizedPatterns(pattern, index_pattern);
    }
    return normalizePattern(pattern);
  }

  if (const auto *op = dyn_cast<Operator>(value)) {
    if (op->getOpcode() == Instruction::BitCast) {
      return normalizePattern(extractAffineAccessPattern(op->getOperand(0)));
    }
    if (op->getOpcode() == Instruction::ZExt ||
        op->getOpcode() == Instruction::SExt) {
      pattern = extractAffineAccessPattern(op->getOperand(0));
      if (pattern.valid) {
        pattern.exact = false;
        pattern.non_affine = true;
      }
      return pattern;
    }
    if (op->getOpcode() == Instruction::Trunc) {
      // Width-changing integer operations are only affine when their input
      // range proves that extension/truncation preserves the mathematical
      // value. No such range proof is available in this local model.
      markNonAffine(pattern);
      return pattern;
    }
    if (op->getOpcode() == Instruction::IntToPtr) {
      return normalizePattern(extractAffineAccessPattern(op->getOperand(0)));
    }
    if (op->getOpcode() == Instruction::PtrToInt) {
      return normalizePattern(extractAffineAccessPattern(op->getOperand(0)));
    }
    if (op->getOpcode() == Instruction::Select) {
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(1));
      AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(2));
      if (!lhs.valid || !rhs.valid) {
        markNonAffine(pattern);
        return pattern;
      }
      const bool identical =
          lhs.exact && rhs.exact && lhs.constant == rhs.constant &&
          lhs.thread_idx_x == rhs.thread_idx_x &&
          lhs.thread_idx_y == rhs.thread_idx_y &&
          lhs.thread_idx_z == rhs.thread_idx_z &&
          lhs.block_idx_x == rhs.block_idx_x &&
          lhs.block_idx_y == rhs.block_idx_y &&
          lhs.block_idx_z == rhs.block_idx_z && lhs.lane_id == rhs.lane_id;
      if (identical) {
        return lhs;
      }
      markNonAffine(pattern);
      return pattern;
    }
    const auto *overflowing = dyn_cast<OverflowingBinaryOperator>(op);
    const bool no_wrap = overflowing && overflowing->hasNoUnsignedWrap() &&
                         overflowing->hasNoSignedWrap();
    if (op->getOpcode() == Instruction::Add) {
      pattern = mergeNormalizedPatterns(
          extractAffineAccessPattern(op->getOperand(0)),
          extractAffineAccessPattern(op->getOperand(1)));
      if (pattern.valid && !no_wrap) {
        pattern.exact = false;
        pattern.non_affine = true;
      }
      return pattern;
    }
    if (op->getOpcode() == Instruction::Sub) {
      pattern = mergeNormalizedPatterns(
          extractAffineAccessPattern(op->getOperand(0)),
          extractAffineAccessPattern(op->getOperand(1)), true);
      if (pattern.valid && !no_wrap) {
        pattern.exact = false;
        pattern.non_affine = true;
      }
      return pattern;
    }
    if (op->getOpcode() == Instruction::And ||
        op->getOpcode() == Instruction::Or ||
        op->getOpcode() == Instruction::Xor) {
      markNonAffine(pattern);
      return pattern;
    }
    if (op->getOpcode() == Instruction::Mul) {
      auto lhs_const = evaluateConstantInt(op->getOperand(0));
      auto rhs_const = evaluateConstantInt(op->getOperand(1));
      if (lhs_const && !rhs_const) {
        AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(1));
        if (!rhs.valid) {
          return pattern;
        }
        const int64_t scale = *lhs_const;
        if (tryScalePattern(rhs, scale)) {
          if (!no_wrap) {
            rhs.exact = false;
            rhs.non_affine = true;
          }
          return rhs;
        }
        markNonAffine(pattern);
        return pattern;
      }
      if (!lhs_const && rhs_const) {
        AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
        if (!lhs.valid) {
          return pattern;
        }
        const int64_t scale = *rhs_const;
        if (tryScalePattern(lhs, scale)) {
          if (!no_wrap) {
            lhs.exact = false;
            lhs.non_affine = true;
          }
          return lhs;
        }
        markNonAffine(pattern);
        return pattern;
      }
    }
    if (op->getOpcode() == Instruction::Shl) {
      auto shift = evaluateConstantInt(op->getOperand(1));
      const auto *integer_type = dyn_cast<IntegerType>(op->getType());
      const auto *overflowing = dyn_cast<OverflowingBinaryOperator>(op);
      if (!shift || !integer_type || *shift < 0 ||
          static_cast<uint64_t>(*shift) >= integer_type->getBitWidth() ||
          !overflowing) {
        markNonAffine(pattern);
        return pattern;
      }
      APInt scale(integer_type->getBitWidth(), 1);
      scale <<= static_cast<unsigned>(*shift);
      if (!scale.isSignedIntN(64)) {
        markNonAffine(pattern);
        return pattern;
      }
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
      if (!lhs.valid || !tryScalePattern(lhs, scale.getSExtValue())) {
        markNonAffine(pattern);
        return pattern;
      }
      if (!overflowing->hasNoUnsignedWrap() ||
          !overflowing->hasNoSignedWrap()) {
        lhs.exact = false;
        lhs.non_affine = true;
      }
      return lhs;
    }
    if (op->getOpcode() == Instruction::SDiv ||
        op->getOpcode() == Instruction::UDiv ||
        op->getOpcode() == Instruction::AShr ||
        op->getOpcode() == Instruction::LShr) {
      auto divisor = evaluateConstantInt(op->getOperand(1));
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
      if (!divisor || !lhs.valid || *divisor < 0) {
        markNonAffine(pattern);
        return pattern;
      }
      int64_t scale = *divisor;
      if (op->getOpcode() == Instruction::AShr ||
          op->getOpcode() == Instruction::LShr) {
        const auto *integer_type = dyn_cast<IntegerType>(op->getType());
        if (!integer_type ||
            static_cast<uint64_t>(*divisor) >= integer_type->getBitWidth() ||
            *divisor >= 63) {
          markNonAffine(pattern);
          return pattern;
        }
        scale = int64_t{1} << *divisor;
      }
      if (scale == 0 || !tryDividePattern(lhs, scale)) {
        markNonAffine(pattern);
        return pattern;
      }
      lhs.exact = false;
      lhs.non_affine = true;
      return lhs;
    }
  }

  markNonAffine(pattern);
  return pattern;
}

CanonicalAffineAccessPattern CUDASymbolicModel::normalizeAffineAccessPattern(
    const AffineAccessPattern &pattern,
    const std::array<int64_t, 3> &block_dims,
    const std::array<int64_t, 3> &grid_dims) {
  return canonicalizePattern(normalizePattern(pattern), block_dims, grid_dims);
}

bool AffineAccessPattern::isZero() const {
  return constant == 0 && thread_idx_x == 0 && thread_idx_y == 0 &&
         thread_idx_z == 0 && block_idx_x == 0 && block_idx_y == 0 &&
         block_idx_z == 0 && lane_id == 0;
}

bool AffineAccessPattern::isDivisibleBy(int64_t divisor) const {
  if (divisor == 0) {
    return false;
  }
  return constant % divisor == 0 && thread_idx_x % divisor == 0 &&
         thread_idx_y % divisor == 0 && thread_idx_z % divisor == 0 &&
         block_idx_x % divisor == 0 && block_idx_y % divisor == 0 &&
         block_idx_z % divisor == 0 && lane_id % divisor == 0;
}

void AffineAccessPattern::divideBy(int64_t divisor) {
  if (divisor == 0) {
    return;
  }
  constant /= divisor;
  thread_idx_x /= divisor;
  thread_idx_y /= divisor;
  thread_idx_z /= divisor;
  block_idx_x /= divisor;
  block_idx_y /= divisor;
  block_idx_z /= divisor;
  lane_id /= divisor;
}

SymbolicDimension CUDASymbolicModel::classifyDimension(const Value *value) {
  SymbolicDimension dim;
  dim.value = value;
  if (!value) {
    return dim;
  }
  if (const auto constant = evaluateConstantInt(value)) {
    dim.kind = SymbolicValueKind::Constant;
    dim.constant = static_cast<uint64_t>(*constant);
    return dim;
  }
  if (classifyBuiltin(value) != BuiltinKind::None) {
    dim.kind = SymbolicValueKind::DerivedFromBuiltin;
    return dim;
  }
  if (isa<Argument>(value) || isa<Instruction>(value)) {
    dim.kind = SymbolicValueKind::Symbolic;
    return dim;
  }
  return dim;
}

UniformityClass CUDASymbolicModel::classifyUniformity(const Value *value) {
  if (!value) {
    return UniformityClass::ThreadVarying;
  }

  const BuiltinKind builtin = classifyBuiltin(value);
  switch (builtin) {
  case BuiltinKind::ThreadIdxX:
  case BuiltinKind::ThreadIdxY:
  case BuiltinKind::ThreadIdxZ:
  case BuiltinKind::LaneId:
  case BuiltinKind::LaneMaskLt:
  case BuiltinKind::LaneMaskLe:
  case BuiltinKind::LaneMaskGt:
  case BuiltinKind::LaneMaskGe:
  case BuiltinKind::Shuffle:
  case BuiltinKind::ShuffleDown:
  case BuiltinKind::ShuffleUp:
  case BuiltinKind::ShuffleXor:
    return UniformityClass::ThreadVarying;
  case BuiltinKind::BlockIdxX:
  case BuiltinKind::BlockIdxY:
  case BuiltinKind::BlockIdxZ:
  case BuiltinKind::BlockDimX:
  case BuiltinKind::BlockDimY:
  case BuiltinKind::BlockDimZ:
  case BuiltinKind::GridDimX:
  case BuiltinKind::GridDimY:
  case BuiltinKind::GridDimZ:
  case BuiltinKind::WarpSize:
    return UniformityClass::BlockUniform;
  default:
    break;
  }

  if (builtin == BuiltinKind::VoteAny || builtin == BuiltinKind::VoteAll ||
      builtin == BuiltinKind::VoteBallot) {
    return UniformityClass::WarpUniform;
  }

  if (isa<Constant>(value)) {
    return UniformityClass::BlockUniform;
  }

  if (const auto *inst = dyn_cast<Instruction>(value)) {
    if (isa<LoadInst>(inst) || isa<CallBase>(inst)) {
      return UniformityClass::ThreadVarying;
    }
    UniformityClass result = UniformityClass::BlockUniform;
    for (const Value *operand : inst->operands()) {
      result = mergeUniformity(result, classifyUniformity(operand));
    }
    return result;
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    UniformityClass result = UniformityClass::BlockUniform;
    for (const Value *operand : ce->operands()) {
      result = mergeUniformity(result, classifyUniformity(operand));
    }
    return result;
  }

  return UniformityClass::Unknown;
}

ParticipationScope
CUDASymbolicModel::classifyParticipation(const Value *value) {
  AffineAccessPattern pattern = extractAffineAccessPattern(value);
  return pattern.valid ? pattern.participation : ParticipationScope::Unknown;
}

int64_t AffineAccessPattern::linearize(int64_t x, int64_t y, int64_t z,
                                       int64_t dim_x, int64_t dim_y,
                                       int64_t dim_z) {
  if (dim_x <= 0)
    dim_x = 1;
  if (dim_y <= 0)
    dim_y = 1;
  if (dim_z <= 0)
    dim_z = 1;
  return (z * dim_y + y) * dim_x + x;
}

void AffineAccessPattern::delinearize(int64_t linear, int64_t dim_x,
                                      int64_t dim_y, int64_t &x, int64_t &y,
                                      int64_t &z) {
  if (dim_x <= 0)
    dim_x = 1;
  if (dim_y <= 0)
    dim_y = 1;
  x = linear % dim_x;
  int64_t rest = linear / dim_x;
  y = rest % dim_y;
  z = rest / dim_y;
}

} // namespace concurrency::cuda
