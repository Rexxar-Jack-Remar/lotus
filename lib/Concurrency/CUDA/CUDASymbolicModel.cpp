#include "Concurrency/CUDA/CUDASymbolicModel.h"

#include <algorithm>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
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
  return subtract ? (*lhs - *rhs) : (*lhs + *rhs);
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
  dst.constant =
      subtract ? lhs.constant - rhs.constant : lhs.constant + rhs.constant;
  dst.thread_idx_x = subtract ? lhs.thread_idx_x - rhs.thread_idx_x
                              : lhs.thread_idx_x + rhs.thread_idx_x;
  dst.thread_idx_y = subtract ? lhs.thread_idx_y - rhs.thread_idx_y
                              : lhs.thread_idx_y + rhs.thread_idx_y;
  dst.thread_idx_z = subtract ? lhs.thread_idx_z - rhs.thread_idx_z
                              : lhs.thread_idx_z + rhs.thread_idx_z;
  dst.block_idx_x = subtract ? lhs.block_idx_x - rhs.block_idx_x
                             : lhs.block_idx_x + rhs.block_idx_x;
  dst.block_idx_y = subtract ? lhs.block_idx_y - rhs.block_idx_y
                             : lhs.block_idx_y + rhs.block_idx_y;
  dst.block_idx_z = subtract ? lhs.block_idx_z - rhs.block_idx_z
                             : lhs.block_idx_z + rhs.block_idx_z;
  dst.lane_id =
      subtract ? lhs.lane_id - rhs.lane_id : lhs.lane_id + rhs.lane_id;
  dst.valid = true;
  dst.exact = lhs.exact && rhs.exact;
  dst.non_affine = lhs.non_affine || rhs.non_affine;
  dst.participation = std::max(lhs.participation, rhs.participation);
}

static void scalePattern(AffineAccessPattern &pattern, int64_t scale) {
  pattern.constant *= scale;
  pattern.thread_idx_x *= scale;
  pattern.thread_idx_y *= scale;
  pattern.thread_idx_z *= scale;
  pattern.block_idx_x *= scale;
  pattern.block_idx_y *= scale;
  pattern.block_idx_z *= scale;
  pattern.lane_id *= scale;
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

static AffineAccessPattern mergeNormalizedPatterns(const AffineAccessPattern &lhs,
                                                   const AffineAccessPattern &rhs,
                                                   bool subtract = false) {
  AffineAccessPattern merged;
  mergePatterns(merged, lhs, rhs, subtract);
  return normalizePattern(merged);
}

static bool tryScalePattern(AffineAccessPattern &pattern, int64_t scale) {
  if (!pattern.valid) {
    return false;
  }
  scalePattern(pattern, scale);
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
  canonical.linear_thread =
      pattern.thread_idx_x + pattern.thread_idx_y * block_x +
      pattern.thread_idx_z * block_x * block_y;
  canonical.linear_block =
      pattern.block_idx_x + pattern.block_idx_y * grid_x +
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
  return std::max(lhs, rhs);
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
                            BuiltinKind::ThreadIdxZ, BuiltinKind::LaneId});
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
        return (*lhs) * (*rhs);
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
        return inst->getOpcode() == Instruction::Mul ? (*lhs) * (*rhs)
                                                     : (*lhs) << (*rhs);
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

    Type *element_type = gep->getSourceElementType();
    const auto *idx_it = gep->idx_begin();
    for (; idx_it != gep->idx_end(); ++idx_it) {
      AffineAccessPattern index_pattern = extractAffineAccessPattern(*idx_it);
      if (!index_pattern.valid) {
        pattern.non_affine = true;
        return normalizePattern(pattern);
      }

      int64_t elem_size = 1;
      if (element_type->isSized()) {
        if (const auto *arr = dyn_cast<ArrayType>(element_type)) {
          element_type = arr->getElementType();
        } else if (const auto *vec = dyn_cast<VectorType>(element_type)) {
          element_type = vec->getElementType();
        } else if (const auto *ptr = dyn_cast<PointerType>(element_type)) {
          element_type = ptr->getPointerElementType();
        }
        if (element_type->isIntegerTy()) {
          elem_size =
              std::max<int64_t>(1, element_type->getIntegerBitWidth() / 8);
        } else if (element_type->isDoubleTy() || element_type->isPointerTy()) {
          elem_size = 8;
        } else if (const auto *arr = dyn_cast<ArrayType>(element_type)) {
          elem_size = static_cast<int64_t>(arr->getNumElements());
          element_type = arr->getElementType();
        }
      }

      scalePattern(index_pattern, elem_size);
      if (!pattern.valid) {
        pattern = index_pattern;
      } else {
        pattern = mergeNormalizedPatterns(pattern, index_pattern);
      }
    }
    return normalizePattern(pattern);
  }

  if (const auto *op = dyn_cast<Operator>(value)) {
    if (op->getOpcode() == Instruction::ZExt ||
        op->getOpcode() == Instruction::SExt ||
        op->getOpcode() == Instruction::Trunc ||
        op->getOpcode() == Instruction::BitCast) {
      return normalizePattern(extractAffineAccessPattern(op->getOperand(0)));
    }
    if (op->getOpcode() == Instruction::Select) {
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(1));
      AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(2));
      if (!lhs.valid || !rhs.valid) {
        markNonAffine(pattern);
        return pattern;
      }
      pattern = lhs;
      pattern.exact = false;
      pattern.non_affine = lhs.non_affine || rhs.non_affine;
      pattern.participation = std::max(lhs.participation, rhs.participation);
      return normalizePattern(pattern);
    }
    if (op->getOpcode() == Instruction::Add) {
      return mergeNormalizedPatterns(extractAffineAccessPattern(op->getOperand(0)),
                                     extractAffineAccessPattern(op->getOperand(1)));
    }
    if (op->getOpcode() == Instruction::Sub) {
      return mergeNormalizedPatterns(extractAffineAccessPattern(op->getOperand(0)),
                                     extractAffineAccessPattern(op->getOperand(1)),
                                     true);
    }
    if (op->getOpcode() == Instruction::And) {
      if (auto mask = evaluateConstantInt(op->getOperand(1))) {
        AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
        if (lhs.valid && lhs.thread_idx_x == 0 && lhs.block_idx_x == 1 &&
            lhs.lane_id == 0) {
          lhs.constant &= *mask;
          return normalizePattern(lhs);
        }
      }
    }
    if (op->getOpcode() == Instruction::Or) {
      auto lhs_const = evaluateConstantInt(op->getOperand(0));
      auto rhs_const = evaluateConstantInt(op->getOperand(1));
      if (lhs_const && !rhs_const) {
        pattern = extractAffineAccessPattern(op->getOperand(1));
        if (pattern.valid) {
          pattern.constant |= *lhs_const;
        }
        return normalizePattern(pattern);
      }
      if (!lhs_const && rhs_const) {
        pattern = extractAffineAccessPattern(op->getOperand(0));
        if (pattern.valid) {
          pattern.constant |= *rhs_const;
        }
        return normalizePattern(pattern);
      }
    }
    if (op->getOpcode() == Instruction::Mul ||
        op->getOpcode() == Instruction::Shl) {
      auto lhs_const = evaluateConstantInt(op->getOperand(0));
      auto rhs_const = evaluateConstantInt(op->getOperand(1));
      if (lhs_const && !rhs_const) {
        AffineAccessPattern rhs = extractAffineAccessPattern(op->getOperand(1));
        if (!rhs.valid) {
          return pattern;
        }
        const int64_t scale = *lhs_const;
        if (tryScalePattern(rhs, scale)) {
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
        const int64_t scale = op->getOpcode() == Instruction::Shl
                                  ? (int64_t{1} << *rhs_const)
                                  : *rhs_const;
        if (tryScalePattern(lhs, scale)) {
          return lhs;
        }
        markNonAffine(pattern);
        return pattern;
      }
    }
    if (op->getOpcode() == Instruction::SDiv ||
        op->getOpcode() == Instruction::UDiv ||
        op->getOpcode() == Instruction::AShr ||
        op->getOpcode() == Instruction::LShr) {
      auto divisor = evaluateConstantInt(op->getOperand(1));
      AffineAccessPattern lhs = extractAffineAccessPattern(op->getOperand(0));
      if (divisor && lhs.valid && *divisor != 0) {
        const int64_t scale = (op->getOpcode() == Instruction::AShr ||
                               op->getOpcode() == Instruction::LShr)
                                  ? (int64_t{1} << *divisor)
                                  : *divisor;
        if (tryDividePattern(lhs, scale)) {
          return lhs;
        }
        markNonAffine(lhs);
        return lhs;
      }
    }
  }

  markNonAffine(pattern);
  return pattern;
}

CanonicalAffineAccessPattern CUDASymbolicModel::normalizeAffineAccessPattern(
    const AffineAccessPattern &pattern, const std::array<int64_t, 3> &block_dims,
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
    return UniformityClass::WarpUniform;
  }

  const BuiltinKind builtin = classifyBuiltin(value);
  switch (builtin) {
  case BuiltinKind::ThreadIdxX:
  case BuiltinKind::ThreadIdxY:
  case BuiltinKind::ThreadIdxZ:
  case BuiltinKind::LaneId:
    return UniformityClass::ThreadVarying;
  case BuiltinKind::Shuffle:
  case BuiltinKind::ShuffleDown:
  case BuiltinKind::ShuffleUp:
  case BuiltinKind::ShuffleXor:
    return UniformityClass::WarpUniform;
  case BuiltinKind::VoteAny:
  case BuiltinKind::VoteAll:
  case BuiltinKind::VoteBallot:
  case BuiltinKind::LaneMaskLt:
  case BuiltinKind::LaneMaskLe:
  case BuiltinKind::LaneMaskGt:
  case BuiltinKind::LaneMaskGe:
  case BuiltinKind::WarpSize:
    return UniformityClass::WarpUniform;
  case BuiltinKind::BlockIdxX:
  case BuiltinKind::BlockIdxY:
  case BuiltinKind::BlockIdxZ:
    return UniformityClass::BlockUniform;
  case BuiltinKind::BlockDimX:
  case BuiltinKind::BlockDimY:
  case BuiltinKind::BlockDimZ:
  case BuiltinKind::GridDimX:
  case BuiltinKind::GridDimY:
  case BuiltinKind::GridDimZ:
    return UniformityClass::WarpUniform;
  default:
    break;
  }

  if (isa<Constant>(value)) {
    return UniformityClass::WarpUniform;
  }

  if (const auto *inst = dyn_cast<Instruction>(value)) {
    UniformityClass result = UniformityClass::WarpUniform;
    for (const Value *operand : inst->operands()) {
      result = mergeUniformity(result, classifyUniformity(operand));
    }
    return result;
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    UniformityClass result = UniformityClass::WarpUniform;
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
