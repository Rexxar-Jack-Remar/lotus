/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/InterAffineEqualities.h"

#include "Dataflow/NPA/Analyses/InterEngine.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

using D = AffineRelationDomain;
using Exp = Exp0<D>;
using E = E0<D>;
using Relation = D::value_type;
using Component = AffineRelationComponent;
using Row = std::vector<llvm::APInt>;

struct PartialConstant {
  unsigned bits = 0;
  llvm::APInt value;
};

bool isTrackedScalar(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64;
}

bool getConstantIntValue(const llvm::Value *V, int64_t &out) {
  auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
  if (!CI || CI->getBitWidth() > 64)
    return false;
  out = CI->getBitWidth() == 1 ? static_cast<int64_t>(CI->getZExtValue())
                               : CI->getSExtValue();
  return true;
}

int64_t wrapToBitWidth(int64_t value, unsigned bitWidth) {
  llvm::APInt wrapped(bitWidth, static_cast<uint64_t>(value), true);
  return bitWidth == 1 ? static_cast<int64_t>(wrapped.getZExtValue())
                       : wrapped.getSExtValue();
}

AffineExpr topExpr() { return {}; }

AffineExpr constExpr(int64_t value) {
  AffineExpr out;
  out.top = false;
  out.constant = value;
  return out;
}

AffineExpr normalizeExpr(AffineExpr expr, unsigned bitWidth) {
  if (expr.top)
    return expr;
  expr.constant = wrapToBitWidth(expr.constant, bitWidth);
  for (auto It = expr.terms.begin(); It != expr.terms.end();) {
    It->second = wrapToBitWidth(It->second, bitWidth);
    if (It->second == 0)
      It = expr.terms.erase(It);
    else
      ++It;
  }
  return expr;
}

AffineExpr addExpr(AffineExpr lhs, const AffineExpr &rhs, unsigned bitWidth) {
  if (lhs.top || rhs.top)
    return topExpr();
  lhs.constant += rhs.constant;
  for (const auto &term : rhs.terms)
    lhs.terms[term.first] += term.second;
  return normalizeExpr(std::move(lhs), bitWidth);
}

AffineExpr scaleExpr(AffineExpr expr, int64_t factor, unsigned bitWidth) {
  if (expr.top)
    return topExpr();
  expr.constant *= factor;
  for (auto &term : expr.terms)
    term.second *= factor;
  return normalizeExpr(std::move(expr), bitWidth);
}

AffineExpr variableExpr(const llvm::Value *value) {
  AffineExpr out;
  out.top = false;
  out.terms[value] = 1;
  return out;
}

std::optional<unsigned> integerBitWidth(const llvm::Value *value) {
  auto *Ty = value ? value->getType() : nullptr;
  if (!Ty || !Ty->isIntegerTy() || Ty->getIntegerBitWidth() > 64)
    return std::nullopt;
  return Ty->getIntegerBitWidth();
}

bool isEquivalentAffineExpr(const AffineExpr &lhs, const AffineExpr &rhs,
                            unsigned bitWidth) {
  return normalizeExpr(lhs, bitWidth) == normalizeExpr(rhs, bitWidth);
}

std::optional<AffineExpr>
affineExprForValue(const llvm::Value *value,
                   std::unordered_set<const llvm::Value *> &visiting);

PartialConstant partialConstantForExpr(const AffineExpr &expr, unsigned bitWidth);

std::optional<AffineExpr> affineExprForBinary(const llvm::BinaryOperator &BinOp,
                                              std::unordered_set<const llvm::Value *>
                                                  &visiting) {
  auto width = integerBitWidth(&BinOp);
  if (!width)
    return std::nullopt;

  auto getExpr = [&](const llvm::Value *operand) {
    return affineExprForValue(operand, visiting);
  };

  int64_t lhsConst = 0;
  int64_t rhsConst = 0;
  auto lhsExpr = getExpr(BinOp.getOperand(0));
  auto rhsExpr = getExpr(BinOp.getOperand(1));

  switch (BinOp.getOpcode()) {
  case llvm::Instruction::Add:
    if (lhsExpr && rhsExpr)
      return addExpr(*lhsExpr, *rhsExpr, *width);
    break;
  case llvm::Instruction::Sub:
    if (lhsExpr && rhsExpr)
      return addExpr(*lhsExpr, scaleExpr(*rhsExpr, -1, *width), *width);
    break;
  case llvm::Instruction::Mul:
    if (getConstantIntValue(BinOp.getOperand(0), lhsConst) && rhsExpr)
      return scaleExpr(*rhsExpr, wrapToBitWidth(lhsConst, *width), *width);
    if (getConstantIntValue(BinOp.getOperand(1), rhsConst) && lhsExpr)
      return scaleExpr(*lhsExpr, wrapToBitWidth(rhsConst, *width), *width);
    break;
  case llvm::Instruction::Shl:
    if (!lhsExpr || !getConstantIntValue(BinOp.getOperand(1), rhsConst) ||
        rhsConst < 0 || static_cast<unsigned>(rhsConst) >= *width) {
      break;
    }
    return scaleExpr(
        *lhsExpr,
        llvm::APInt(*width, 1).shl(static_cast<unsigned>(rhsConst)).getSExtValue(),
        *width);
  default:
    break;
  }

  return std::nullopt;
}

std::optional<AffineExpr> exactQuotientByPowerOfTwo(const AffineExpr &expr,
                                                    unsigned bitWidth,
                                                    unsigned shift) {
  if (expr.top || shift >= bitWidth)
    return std::nullopt;

  PartialConstant known = partialConstantForExpr(expr, bitWidth);
  if (known.bits < shift)
    return std::nullopt;

  AffineExpr out;
  out.top = false;
  llvm::APInt divisor(bitWidth, 1);
  divisor <<= shift;
  llvm::APInt constant(bitWidth, static_cast<uint64_t>(expr.constant), true);
  out.constant = constant.udiv(divisor).getSExtValue();
  for (const auto &term : expr.terms) {
    llvm::APInt coeff(bitWidth, static_cast<uint64_t>(term.second), true);
    out.terms[term.first] = coeff.udiv(divisor).getSExtValue();
  }
  return normalizeExpr(std::move(out), bitWidth);
}

std::optional<AffineExpr> affineExprForCast(const llvm::CastInst &Cast,
                                            std::unordered_set<const llvm::Value *>
                                                &visiting) {
  auto dstWidth = integerBitWidth(&Cast);
  auto srcWidth = integerBitWidth(Cast.getOperand(0));
  if (!dstWidth || !srcWidth)
    return std::nullopt;

  auto operandExpr = affineExprForValue(Cast.getOperand(0), visiting);
  if (!operandExpr)
    return std::nullopt;

  auto castConstant = [&](int64_t value) -> AffineExpr {
    llvm::APInt bits(*srcWidth, static_cast<uint64_t>(value), true);
    switch (Cast.getOpcode()) {
    case llvm::Instruction::SExt:
      return constExpr(bits.sext(*dstWidth).getSExtValue());
    case llvm::Instruction::ZExt:
      return constExpr(bits.zext(*dstWidth).getSExtValue());
    case llvm::Instruction::Trunc:
      return constExpr(bits.trunc(*dstWidth).getSExtValue());
    default:
      return topExpr();
    }
  };

  if (operandExpr->terms.empty()) {
    switch (Cast.getOpcode()) {
    case llvm::Instruction::SExt:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::Trunc:
      return castConstant(operandExpr->constant);
    default:
      break;
    }
  }

  switch (Cast.getOpcode()) {
  case llvm::Instruction::SExt:
    if (*srcWidth == *dstWidth)
      return normalizeExpr(*operandExpr, *dstWidth);
    if (*srcWidth == 1)
      return scaleExpr(*operandExpr, -1, *dstWidth);
    break;
  case llvm::Instruction::ZExt:
    if (*srcWidth == *dstWidth)
      return normalizeExpr(*operandExpr, *dstWidth);
    if (*srcWidth == 1)
      return normalizeExpr(*operandExpr, *dstWidth);
    break;
  case llvm::Instruction::Trunc:
    if (*srcWidth == *dstWidth)
      return normalizeExpr(*operandExpr, *dstWidth);
    break;
  default:
    break;
  }

  return std::nullopt;
}

std::optional<AffineExpr>
affineExprForValue(const llvm::Value *value,
                   std::unordered_set<const llvm::Value *> &visiting) {
  int64_t constant = 0;
  if (getConstantIntValue(value, constant)) {
    auto width = integerBitWidth(value);
    return width ? std::optional<AffineExpr>(constExpr(wrapToBitWidth(
                       constant, *width)))
                 : std::nullopt;
  }

  if (!isTrackedScalar(value))
    return std::nullopt;
  if (!visiting.insert(value).second)
    return variableExpr(value);

  std::optional<AffineExpr> out;
  if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    out = affineExprForCast(*Cast, visiting);
  } else if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    out = affineExprForBinary(*BinOp, visiting);
  } else if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    int64_t cond = 0;
    if (getConstantIntValue(Select->getCondition(), cond))
      out = affineExprForValue(cond != 0 ? Select->getTrueValue()
                                         : Select->getFalseValue(),
                               visiting);
    else {
      auto trueExpr = affineExprForValue(Select->getTrueValue(), visiting);
      auto falseExpr = affineExprForValue(Select->getFalseValue(), visiting);
      auto width = integerBitWidth(value);
      if (trueExpr && falseExpr && width &&
          isEquivalentAffineExpr(*trueExpr, *falseExpr, *width)) {
        out = normalizeExpr(*trueExpr, *width);
      }
    }
  }

  visiting.erase(value);
  if (out)
    return out;
  if (llvm::isa<llvm::CastInst>(value) ||
      llvm::isa<llvm::BinaryOperator>(value) ||
      llvm::isa<llvm::SelectInst>(value)) {
    return std::nullopt;
  }
  return variableExpr(value);
}

std::optional<AffineExpr> affineExprForValue(const llvm::Value *value) {
  std::unordered_set<const llvm::Value *> visiting;
  return affineExprForValue(value, visiting);
}

Relation equalityConstraintForExprs(const AffineExpr &lhs, const AffineExpr &rhs,
                                    unsigned bitWidth) {
  std::unordered_map<const llvm::Value *, int64_t> coeffs = lhs.terms;
  for (const auto &term : rhs.terms)
    coeffs[term.first] -= term.second;

  std::vector<std::pair<const llvm::Value *, int64_t>> terms;
  for (const auto &term : coeffs) {
    int64_t coeff = wrapToBitWidth(term.second, bitWidth);
    if (coeff != 0)
      terms.emplace_back(term.first, coeff);
  }

  int64_t constant = wrapToBitWidth(rhs.constant - lhs.constant, bitWidth);
  if (terms.empty() && constant == 0)
    return D::identity();
  return D::addStateConstraint(D::identity(), constant, terms);
}

std::optional<bool>
evaluateAffineComparison(const AffineExpr &lhs, const AffineExpr &rhs,
                         llvm::CmpInst::Predicate predicate,
                         unsigned bitWidth) {
  AffineExpr diff = normalizeExpr(addExpr(lhs, scaleExpr(rhs, -1, bitWidth),
                                          bitWidth),
                                  bitWidth);
  if (!diff.terms.empty())
    return std::nullopt;

  llvm::APInt lhsValue(bitWidth, static_cast<uint64_t>(lhs.constant), true);
  llvm::APInt rhsValue(bitWidth, static_cast<uint64_t>(rhs.constant), true);
  return llvm::ICmpInst::compare(lhsValue, rhsValue, predicate);
}

std::optional<bool> compareEquivalentAffineExprs(llvm::CmpInst::Predicate predicate) {
  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::ICMP_UGE:
  case llvm::CmpInst::ICMP_ULE:
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::ICMP_SLE:
    return true;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::ICMP_UGT:
  case llvm::CmpInst::ICMP_ULT:
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::ICMP_SLT:
    return false;
  default:
    return std::nullopt;
  }
}

Relation identityWithConditionValue(const llvm::Value *condition, bool value) {
  return D::isTrackedValue(condition)
             ? D::addPrecondition(D::identity(), condition, value ? 1 : 0)
             : D::identity();
}

Relation equalityConstraintForOperands(const llvm::Value *lhs,
                                       const llvm::Value *rhs) {
  auto width = integerBitWidth(lhs);
  auto lhsExpr = affineExprForValue(lhs);
  auto rhsExpr = affineExprForValue(rhs);
  if (width && lhsExpr && rhsExpr)
    return equalityConstraintForExprs(*lhsExpr, *rhsExpr, *width);

  int64_t constant = 0;
  if (D::isTrackedValue(lhs) && getConstantIntValue(rhs, constant)) {
    return D::addStateConstraint(
        D::identity(), wrapToBitWidth(constant, D::bitWidthOf(lhs)), {{lhs, 1}});
  }
  if (D::isTrackedValue(rhs) && getConstantIntValue(lhs, constant)) {
    return D::addStateConstraint(
        D::identity(), wrapToBitWidth(constant, D::bitWidthOf(rhs)), {{rhs, 1}});
  }
  if (D::isTrackedValue(lhs) && D::isTrackedValue(rhs)) {
    return D::addStateConstraint(D::identity(), 0, {{lhs, 1}, {rhs, -1}});
  }
  return D::identity();
}

Relation conditionRefinement(const llvm::Value *condition, bool expectedValue) {
  Relation relation = identityWithConditionValue(condition, expectedValue);
  auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(condition);
  if (!Cmp)
    return relation;

  Relation operandRefinement = D::identity();
  switch (Cmp->getPredicate()) {
  case llvm::CmpInst::ICMP_EQ:
    if (expectedValue) {
      operandRefinement = equalityConstraintForOperands(Cmp->getOperand(0),
                                                        Cmp->getOperand(1));
    }
    break;
  case llvm::CmpInst::ICMP_NE:
    if (!expectedValue) {
      operandRefinement = equalityConstraintForOperands(Cmp->getOperand(0),
                                                        Cmp->getOperand(1));
    }
    break;
  default:
    break;
  }
  return D::extend(operandRefinement, relation);
}

std::optional<bool> evaluateConditionConstant(const llvm::Value *condition) {
  int64_t constant = 0;
  if (getConstantIntValue(condition, constant))
    return constant != 0;

  auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(condition);
  if (!Cmp)
    return std::nullopt;

  if (auto width = integerBitWidth(Cmp->getOperand(0))) {
    auto lhsExpr = affineExprForValue(Cmp->getOperand(0));
    auto rhsExpr = affineExprForValue(Cmp->getOperand(1));
    if (lhsExpr && rhsExpr) {
      if (isEquivalentAffineExpr(*lhsExpr, *rhsExpr, *width))
        return compareEquivalentAffineExprs(Cmp->getPredicate());
      if (auto result = evaluateAffineComparison(*lhsExpr, *rhsExpr,
                                                 Cmp->getPredicate(), *width)) {
        return result;
      }
    }
  }

  int64_t lhs = 0, rhs = 0;
  if (!getConstantIntValue(Cmp->getOperand(0), lhs) ||
      !getConstantIntValue(Cmp->getOperand(1), rhs)) {
    return std::nullopt;
  }

  llvm::APInt lhsValue(Cmp->getOperand(0)->getType()->getIntegerBitWidth(),
                       static_cast<uint64_t>(lhs), true);
  llvm::APInt rhsValue(Cmp->getOperand(1)->getType()->getIntegerBitWidth(),
                       static_cast<uint64_t>(rhs), true);
  return llvm::ICmpInst::compare(lhsValue, rhsValue, Cmp->getPredicate());
}

std::optional<int64_t> evaluateSwitchConstant(const llvm::Value *condition) {
  int64_t constant = 0;
  if (getConstantIntValue(condition, constant))
    return constant;

  auto width = integerBitWidth(condition);
  auto expr = affineExprForValue(condition);
  if (!width || !expr || expr->top || !expr->terms.empty())
    return std::nullopt;
  return wrapToBitWidth(expr->constant, *width);
}

unsigned countTrailingOnes(const llvm::APInt &value) {
  unsigned count = 0;
  for (; count < value.getBitWidth() && value[count]; ++count) {
  }
  return count;
}

std::optional<unsigned> exactPowerOfTwoLog(const llvm::APInt &value) {
  if (value.isZero() || !value.isPowerOf2())
    return std::nullopt;
  return value.exactLogBase2();
}

PartialConstant partialConstantForExpr(const AffineExpr &expr,
                                       unsigned bitWidth) {
  PartialConstant out{bitWidth, llvm::APInt(bitWidth, expr.constant, true)};
  if (expr.top) {
    out.bits = 0;
    return out;
  }

  for (const auto &term : expr.terms) {
    llvm::APInt coeff(bitWidth, static_cast<uint64_t>(term.second), true);
    out.bits = std::min(out.bits, coeff.countTrailingZeros());
  }
  if (out.bits < bitWidth) {
    if (out.bits == 0)
      out.value = llvm::APInt(bitWidth, 0);
    else {
      llvm::APInt mask = llvm::APInt::getLowBitsSet(bitWidth, out.bits);
      out.value &= mask;
    }
  }
  return out;
}

Relation congruenceAssignmentForConstant(const llvm::Value *dest,
                                         unsigned modulusBits,
                                         const llvm::APInt &constant) {
  return D::makeAffineCongruenceAssignment(dest, modulusBits,
                                           constant.getSExtValue(), {});
}

llvm::APInt lowBitsValue(const llvm::APInt &value, unsigned bits) {
  if (bits == 0)
    return llvm::APInt(value.getBitWidth(), 0);
  return value & llvm::APInt::getLowBitsSet(value.getBitWidth(), bits);
}

AffineExpr addConstant(AffineExpr expr, int64_t constant, unsigned bitWidth) {
  return addExpr(std::move(expr), constExpr(constant), bitWidth);
}

std::vector<std::pair<const llvm::Value *, int64_t>>
termsForExpr(const AffineExpr &expr) {
  std::vector<std::pair<const llvm::Value *, int64_t>> terms;
  terms.reserve(expr.terms.size());
  for (const auto &term : expr.terms)
    terms.emplace_back(term.first, term.second);
  return terms;
}

Relation assignmentForExpr(const llvm::Value *dest, const AffineExpr &expr) {
  return D::makeAffineAssignment(
      dest, wrapToBitWidth(expr.constant, D::bitWidthOf(dest)),
      termsForExpr(expr));
}

Relation congruenceAssignmentForExpr(const llvm::Value *dest,
                                     unsigned modulusBits,
                                     const AffineExpr &expr) {
  return D::makeAffineCongruenceAssignment(
      dest, modulusBits, wrapToBitWidth(expr.constant, D::bitWidthOf(dest)),
      termsForExpr(expr));
}

unsigned twoAdicRank(const llvm::APInt &value) {
  return value.isZero() ? value.getBitWidth() : value.countTrailingZeros();
}

bool isAssumeLikeCall(const llvm::CallBase &call) {
  auto *callee = call.getCalledFunction();
  if (!callee)
    return false;
  if (callee->getName() == "llvm.assume")
    return true;
  return callee->getName() == "__VERIFIER_assume" ||
         callee->getName() == "__SEA_assume" ||
         callee->getName() == "assume";
}

Relation switchCaseRefinement(const llvm::Value *condition, int64_t caseValue) {
  Relation relation = D::isTrackedValue(condition)
                          ? D::addPrecondition(D::identity(), condition, caseValue)
                          : D::identity();

  auto width = integerBitWidth(condition);
  auto expr = affineExprForValue(condition);
  if (!width || !expr)
    return relation;

  Relation operandRefinement =
      equalityConstraintForExprs(*expr,
                                 constExpr(wrapToBitWidth(caseValue, *width)),
                                 *width);
  return D::extend(operandRefinement, relation);
}

bool isZeroRow(const Row &row) {
  return std::all_of(row.begin(), row.end(),
                     [](const llvm::APInt &entry) { return entry.isZero(); });
}

int leadingIndex(const Row &row) {
  for (size_t i = 0; i < row.size(); ++i) {
    if (!row[i].isZero())
      return static_cast<int>(i);
  }
  return -1;
}

unsigned rankOf(const llvm::APInt &value) {
  return value.isZero() ? value.getBitWidth() : value.countTrailingZeros();
}

llvm::APInt oddInverse(const llvm::APInt &odd) {
  unsigned bitWidth = odd.getBitWidth();
  llvm::APInt inv(bitWidth, 1);
  llvm::APInt two(bitWidth, 2);
  for (unsigned bits = 1; bits < bitWidth; bits <<= 1)
    inv *= (two - odd * inv);
  return inv;
}

void scaleRow(Row &row, const llvm::APInt &factor) {
  for (auto &entry : row)
    entry *= factor;
}

void subtractScaledRow(Row &row, const Row &pivot, const llvm::APInt &factor) {
  for (size_t i = 0; i < row.size(); ++i)
    row[i] -= factor * pivot[i];
}

std::vector<Row> howellize(std::vector<Row> rows) {
  if (rows.empty())
    return rows;
  const unsigned bitWidth = rows.front().front().getBitWidth();
  const size_t cols = rows.front().size();
  size_t nextRow = 0;

  for (size_t col = 0; col < cols; ++col) {
    std::vector<size_t> candidates;
    for (size_t r = nextRow; r < rows.size(); ++r) {
      if (leadingIndex(rows[r]) == static_cast<int>(col))
        candidates.push_back(r);
    }
    if (candidates.empty())
      continue;

    size_t pivotPos = candidates.front();
    for (size_t idx : candidates) {
      if (rankOf(rows[idx][col]) < rankOf(rows[pivotPos][col]))
        pivotPos = idx;
    }

    unsigned pivotRank = rankOf(rows[pivotPos][col]);
    llvm::APInt oddPart = rows[pivotPos][col].lshr(pivotRank);
    scaleRow(rows[pivotPos], oddInverse(oddPart));

    for (size_t idx : candidates) {
      if (idx == pivotPos)
        continue;
      unsigned curRank = rankOf(rows[idx][col]);
      llvm::APInt factor(bitWidth, 1);
      factor <<= (curRank - pivotRank);
      factor *= rows[idx][col].lshr(curRank);
      subtractScaledRow(rows[idx], rows[pivotPos], factor);
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
    auto it =
        std::find_if(rows.begin() + nextRow, rows.end(), [col](const Row &row) {
          return leadingIndex(row) == static_cast<int>(col);
        });
    if (it == rows.end())
      continue;
    std::iter_swap(rows.begin() + nextRow, it);
    const Row pivot = rows[nextRow];

    for (size_t upper = 0; upper < nextRow; ++upper) {
      llvm::APInt factor = rows[upper][col].lshr(pivotRank);
      subtractScaledRow(rows[upper], pivot, factor);
    }

    ++nextRow;
  }

  rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
  return rows;
}

Row reorderToPostPreConst(const Row &row, unsigned vars) {
  Row out(row.front().getBitWidth(), llvm::APInt(row.front().getBitWidth(), 0));
  out.clear();
  out.reserve(2 * vars + 1);
  out.insert(out.end(), row.begin() + vars, row.begin() + 2 * vars);
  out.insert(out.end(), row.begin(), row.begin() + vars);
  out.push_back(row.back());
  return out;
}

AffineEquality normalizeEquality(AffineEquality equality) {
  equality.constant = wrapToBitWidth(equality.constant, equality.bitWidth);
  for (auto It = equality.terms.begin(); It != equality.terms.end();) {
    It->second = wrapToBitWidth(It->second, equality.bitWidth);
    if (It->second == 0)
      It = equality.terms.erase(It);
    else
      ++It;
  }
  return equality;
}

AffineState materializeAffineExpressionsImpl(const Relation &state) {
  AffineState out;
  out.reachable = !state.bottom;
  if (state.bottom)
    return out;

  const auto *vocab = D::getVocabulary();
  if (!vocab)
    return out;
  auto it = state.components.find(D::componentBitWidth());
  if (it == state.components.end())
    return out;

  const unsigned vars = static_cast<unsigned>(vocab->values.size());
  std::vector<Row> rows;
  rows.reserve(it->second.constraints.size());
  for (const Row &row : it->second.constraints)
    rows.push_back(reorderToPostPreConst(row, vars));
  rows = howellize(std::move(rows));

  std::vector<AffineExpr> solved(vars, topExpr());
  std::vector<AffineExpr> preSolved(vars, topExpr());

  bool progress = true;
  while (progress) {
    progress = false;

    for (unsigned preCol = 0; preCol < vars; ++preCol) {
      if (!preSolved[preCol].top)
        continue;
      for (const Row &row : rows) {
        if (row[vars + preCol].isZero())
          continue;
        bool hasPost = false;
        for (unsigned postCol = 0; postCol < vars; ++postCol) {
          if (!row[postCol].isZero()) {
            hasPost = true;
            break;
          }
        }
        if (hasPost)
          continue;

        llvm::APInt coeff = row[vars + preCol];
        if (!coeff[0])
          continue;
        llvm::APInt inv = oddInverse(coeff);
        unsigned actualWidth = vocab->actualBitWidths.at(vocab->values[preCol]);
        AffineExpr expr = constExpr((-row.back() * inv).getSExtValue());
        bool ok = true;
        for (unsigned other = 0; other < vars; ++other) {
          if (other == preCol || row[vars + other].isZero())
            continue;
          if (preSolved[other].top) {
            ok = false;
            break;
          }
          expr = addExpr(std::move(expr),
                         scaleExpr(preSolved[other],
                                   -(row[vars + other] * inv).getSExtValue(),
                                   actualWidth),
                         actualWidth);
        }
        if (ok) {
          preSolved[preCol] = normalizeExpr(std::move(expr), actualWidth);
          progress = true;
          break;
        }
      }
    }

    for (unsigned col = 0; col < vars; ++col) {
      if (!solved[col].top)
        continue;
      for (const Row &row : rows) {
        if (row[col].isZero())
          continue;
        llvm::APInt coeff = row[col];
        if (!coeff[0])
          continue;
        llvm::APInt inv = oddInverse(coeff);
        unsigned actualWidth = vocab->actualBitWidths.at(vocab->values[col]);
        AffineExpr expr = constExpr((-row.back() * inv).getSExtValue());
        bool ok = true;

        for (unsigned postCol = 0; postCol < vars; ++postCol) {
          if (postCol == col || row[postCol].isZero())
            continue;
          if (solved[postCol].top) {
            ok = false;
            break;
          }
          expr = addExpr(std::move(expr),
                         scaleExpr(solved[postCol],
                                   -(row[postCol] * inv).getSExtValue(),
                                   actualWidth),
                         actualWidth);
        }
        if (!ok)
          continue;

        for (unsigned preCol = 0; preCol < vars; ++preCol) {
          if (row[vars + preCol].isZero())
            continue;
          int64_t coeffInt = -(row[vars + preCol] * inv).getSExtValue();
          if (!preSolved[preCol].top) {
            expr = addExpr(std::move(expr),
                           scaleExpr(preSolved[preCol], coeffInt, actualWidth),
                           actualWidth);
          } else {
            expr.terms[vocab->values[preCol]] += coeffInt;
          }
        }
        solved[col] = normalizeExpr(std::move(expr), actualWidth);
        progress = true;
        break;
      }
    }
  }

  for (unsigned i = 0; i < vars; ++i) {
    if (!solved[i].top)
      out.values[vocab->values[i]] = solved[i];
    else if (!preSolved[i].top)
      out.values[vocab->values[i]] = preSolved[i];
  }

  for (const Row &row : rows) {
    AffineEquality equality;
    equality.bitWidth = D::componentBitWidth();
    int64_t lhsConstant = row.back().getSExtValue();

    for (unsigned postCol = 0; postCol < vars; ++postCol) {
      if (!row[postCol].isZero()) {
        equality.terms[vocab->values[postCol]] += row[postCol].getSExtValue();
      }
    }

    for (unsigned preCol = 0; preCol < vars; ++preCol) {
      if (row[vars + preCol].isZero())
        continue;
      int64_t coeff = row[vars + preCol].getSExtValue();
      if (!preSolved[preCol].top) {
        lhsConstant += coeff * preSolved[preCol].constant;
        for (const auto &term : preSolved[preCol].terms)
          equality.terms[term.first] += coeff * term.second;
      } else {
        equality.terms[vocab->values[preCol]] += coeff;
      }
    }

    equality.constant = wrapToBitWidth(-lhsConstant, equality.bitWidth);
    equality = normalizeEquality(std::move(equality));
    if (!equality.terms.empty() || equality.constant != 0)
      out.equalities.push_back(std::move(equality));
  }
  return out;
}

class AffineRelationAnalysis {
public:
  using FactType = Relation;
  using Engine = InterEngine<D, AffineRelationAnalysis>;

  explicit AffineRelationAnalysis(llvm::Module &M) {
    buildVocabulary(M);
    D::configure(&Vocabulary);
  }

  FactType getEntryValue() const { return D::identity(); }

  Relation getEdgeTransfer(const llvm::Instruction &term,
                           const llvm::BasicBlock &succ) const {
    if (auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term)) {
      if (!Branch->isConditional())
        return D::identity();
      bool expectTrue = Branch->getSuccessor(0) == &succ;
      if (auto constant = evaluateConditionConstant(Branch->getCondition()))
        return *constant == expectTrue ? D::identity() : D::zero();
      return conditionRefinement(Branch->getCondition(), expectTrue);
    }
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&term)) {
      if (auto constant = evaluateSwitchConstant(Switch->getCondition())) {
        for (const auto &Case : Switch->cases()) {
          if (Case.getCaseValue()->getSExtValue() == *constant) {
            return Case.getCaseSuccessor() == &succ ? D::identity() : D::zero();
          }
        }
        return Switch->getDefaultDest() == &succ ? D::identity() : D::zero();
      }
      if (!D::isTrackedValue(Switch->getCondition()))
        return D::identity();
      for (const auto &Case : Switch->cases()) {
        if (Case.getCaseSuccessor() == &succ) {
          return switchCaseRefinement(
              Switch->getCondition(),
              static_cast<int64_t>(Case.getCaseValue()->getSExtValue()));
        }
      }
      return D::identity();
    }
    return D::identity();
  }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    auto *FirstPhi = llvm::dyn_cast<llvm::PHINode>(BB.begin());
    if (!FirstPhi)
      return inExpr;

    E result = nullptr;
    for (auto *Pred : predecessors(&BB)) {
      Relation phiTransfer = D::identity();
      for (auto &Inst : BB) {
        auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi)
          break;
        if (!isTrackedScalar(Phi))
          continue;
        Relation assign =
            relationForValue(Phi, Phi->getIncomingValueForBlock(Pred));
        phiTransfer = D::extend(assign, phiTransfer);
      }
      E branch = Exp::hole(Engine::getBlockSymbol(Pred));
      if (auto *PredTerm = Pred->getTerminator()) {
        branch = Exp::seq(getEdgeTransfer(*PredTerm, BB), branch);
      }
      branch = Exp::seq(phiTransfer, branch);
      result = result ? Exp::ndet(result, branch) : branch;
    }
    return result ? result : inExpr;
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::PHINode>(&I))
      return currentPath;
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (isAssumeLikeCall(*Call) && Call->arg_size() >= 1)
        return Exp::seq(conditionRefinement(Call->getArgOperand(0), true),
                        currentPath);
      return currentPath;
    }
    if (I.getType()->isVoidTy())
      return currentPath;
    if (!isTrackedScalar(&I))
      return currentPath;
    return Exp::seq(buildInstructionRelation(I), currentPath);
  }

  Relation getCallEntryTransfer(const llvm::CallBase &Call,
                                const llvm::Function &Callee) {
    Relation relation = D::identity();
    const auto *ParamIt = Callee.arg_begin();
    for (unsigned i = 0; i < Call.arg_size() && ParamIt != Callee.arg_end();
         ++i, ++ParamIt) {
      if (!isTrackedScalar(&*ParamIt))
        continue;
      relation = D::extend(relationForValue(&*ParamIt, Call.getArgOperand(i)),
                           relation);
    }
    return relation;
  }

  Relation getCallReturnTransfer(const llvm::CallBase &Call,
                                 const llvm::Function &Callee) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::identity();

    std::vector<Relation> branches;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret || !Ret->getReturnValue())
        continue;
      branches.push_back(relationForValue(&Call, Ret->getReturnValue()));
    }
    if (branches.empty())
      return D::makeForget(&Call);

    Relation out = branches.front();
    for (size_t i = 1; i < branches.size(); ++i)
      out = D::combine(out, branches[i]);
    return out;
  }

  Relation getCallToReturnTransfer(const llvm::CallBase &Call) {
    return (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
               ? D::identity()
               : D::makeForget(&Call);
  }

  FactType applySummary(const Relation &summary, const FactType &fact) {
    return D::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return D::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return D::equal(lhs, rhs);
  }

  InterAffineEqualities::Result
  buildResult(const typename Engine::Result &engineResult) const {
    InterAffineEqualities::Result result;
    result.status = engineResult.status;
    result.summaries.insert(engineResult.summaries.begin(),
                            engineResult.summaries.end());
    for (const auto &entry : engineResult.blockEntryFacts) {
      result.blockRelations.emplace(entry.first, entry.second);
    }
    return result;
  }

private:
  AffineRelationVocabulary Vocabulary;

  void buildVocabulary(llvm::Module &M) {
    std::unordered_set<const llvm::Value *> seen;
    auto record = [&](const llvm::Value *value) {
      if (seen.insert(value).second)
        Vocabulary.values.push_back(value);
    };
    for (const auto &F : M) {
      if (F.isDeclaration())
        continue;
      for (const auto &Arg : F.args()) {
        if (isTrackedScalar(&Arg))
          record(&Arg);
      }
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (isTrackedScalar(&I)) {
            record(&I);
            Vocabulary.localValues.push_back(&I);
          }
        }
      }
    }
    for (unsigned i = 0; i < Vocabulary.values.size(); ++i) {
      Vocabulary.indices[Vocabulary.values[i]] = i;
      Vocabulary.actualBitWidths[Vocabulary.values[i]] =
          Vocabulary.values[i]->getType()->getIntegerBitWidth();
    }
  }

  Relation relationForValue(const llvm::Value *dest,
                            const llvm::Value *src) const {
    if (auto expr = affineExprForValue(src))
      return assignmentForExpr(dest, *expr);
    return D::makeForget(dest);
  }

  Relation buildInstructionRelation(llvm::Instruction &I) const {
    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I))
      return buildCastRelation(*Cast);
    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I))
      return buildCompareRelation(*Cmp);
    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I))
      return buildSelectRelation(*Select);
    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I))
      return buildBinaryRelation(*BinOp);
    return D::makeForget(&I);
  }

  Relation buildCastRelation(const llvm::CastInst &Cast) const {
    if (auto expr = affineExprForValue(&Cast))
      return assignmentForExpr(&Cast, *expr);
    return D::makeForget(&Cast);
  }

  Relation buildCompareRelation(const llvm::ICmpInst &Cmp) const {
    auto width = integerBitWidth(Cmp.getOperand(0));
    auto lhsExpr = affineExprForValue(Cmp.getOperand(0));
    auto rhsExpr = affineExprForValue(Cmp.getOperand(1));
    if (width && lhsExpr && rhsExpr) {
      if (isEquivalentAffineExpr(*lhsExpr, *rhsExpr, *width)) {
        if (auto result = compareEquivalentAffineExprs(Cmp.getPredicate()))
          return D::makeAffineAssignment(&Cmp, *result ? 1 : 0, {});
      }
      if (auto result =
              evaluateAffineComparison(*lhsExpr, *rhsExpr, Cmp.getPredicate(),
                                       *width)) {
        return D::makeAffineAssignment(&Cmp, *result ? 1 : 0, {});
      }
    }

    int64_t lhs = 0, rhs = 0;
    if (getConstantIntValue(Cmp.getOperand(0), lhs) &&
        getConstantIntValue(Cmp.getOperand(1), rhs)) {
      llvm::APInt lhsValue(Cmp.getOperand(0)->getType()->getIntegerBitWidth(),
                           static_cast<uint64_t>(lhs), true);
      llvm::APInt rhsValue(Cmp.getOperand(1)->getType()->getIntegerBitWidth(),
                           static_cast<uint64_t>(rhs), true);
      bool result =
          llvm::ICmpInst::compare(lhsValue, rhsValue, Cmp.getPredicate());
      return D::makeAffineAssignment(&Cmp, result ? 1 : 0, {});
    }
    if (Cmp.getOperand(0) == Cmp.getOperand(1)) {
      switch (Cmp.getPredicate()) {
      case llvm::CmpInst::ICMP_EQ:
      case llvm::CmpInst::ICMP_UGE:
      case llvm::CmpInst::ICMP_ULE:
      case llvm::CmpInst::ICMP_SGE:
      case llvm::CmpInst::ICMP_SLE:
        return D::makeAffineAssignment(&Cmp, 1, {});
      case llvm::CmpInst::ICMP_NE:
      case llvm::CmpInst::ICMP_UGT:
      case llvm::CmpInst::ICMP_ULT:
      case llvm::CmpInst::ICMP_SGT:
      case llvm::CmpInst::ICMP_SLT:
        return D::makeAffineAssignment(&Cmp, 0, {});
      default:
        break;
      }
    }
    return D::makeForget(&Cmp);
  }

  Relation buildSelectRelation(const llvm::SelectInst &Select) const {
    int64_t cond = 0;
    if (getConstantIntValue(Select.getCondition(), cond))
      return relationForValue(&Select, cond != 0 ? Select.getTrueValue()
                                                 : Select.getFalseValue());
    Relation lhs = relationForValue(&Select, Select.getTrueValue());
    Relation rhs = relationForValue(&Select, Select.getFalseValue());
    if (D::isTrackedValue(Select.getCondition())) {
      lhs = D::addPrecondition(lhs, Select.getCondition(), 1);
      rhs = D::addPrecondition(rhs, Select.getCondition(), 0);
    }
    return D::combine(lhs, rhs);
  }

  Relation buildBitwiseMaskRelation(const llvm::BinaryOperator &BinOp) const {
    auto *L = BinOp.getOperand(0);
    auto *R = BinOp.getOperand(1);
    auto *LC = llvm::dyn_cast<llvm::ConstantInt>(L);
    auto *RC = llvm::dyn_cast<llvm::ConstantInt>(R);
    if (!LC && !RC)
      return D::makeForget(&BinOp);

    const llvm::Value *value = LC ? R : L;
    const llvm::APInt mask = LC ? LC->getValue() : RC->getValue();
    auto expr = affineExprForValue(value);
    if (!expr)
      return D::makeForget(&BinOp);

    unsigned width = BinOp.getType()->getIntegerBitWidth();
    switch (BinOp.getOpcode()) {
    case llvm::Instruction::And: {
      unsigned trailingOnes = countTrailingOnes(mask);
      if (trailingOnes == width)
        return assignmentForExpr(&BinOp, *expr);
      unsigned trailingZeros = mask.countTrailingZeros();
      if (trailingZeros >= width)
        return D::makeAffineAssignment(&BinOp, 0, {});
      if (trailingOnes > 0)
        return congruenceAssignmentForExpr(&BinOp, trailingOnes, *expr);
      if (trailingZeros > 0)
        return D::makeAffineCongruenceAssignment(&BinOp, trailingZeros, 0, {});
      break;
    }
    case llvm::Instruction::Or: {
      unsigned trailingOnes = countTrailingOnes(mask);
      if (trailingOnes >= width)
        return D::makeAffineAssignment(&BinOp, -1, {});
      unsigned trailingZeros = mask.countTrailingZeros();
      if (trailingZeros >= width)
        return assignmentForExpr(&BinOp, *expr);
      if (trailingOnes > 0) {
        llvm::APInt lowOnes(width, 0);
        lowOnes.setLowBits(trailingOnes);
        return D::makeAffineCongruenceAssignment(
            &BinOp, trailingOnes, lowOnes.getSExtValue(), {});
      }
      if (trailingZeros > 0)
        return congruenceAssignmentForExpr(&BinOp, trailingZeros, *expr);
      break;
    }
    case llvm::Instruction::Xor: {
      unsigned trailingZeros = mask.countTrailingZeros();
      if (trailingZeros >= width)
        return assignmentForExpr(&BinOp, *expr);
      unsigned trailingOnes = countTrailingOnes(mask);
      if (trailingOnes >= width)
        return assignmentForExpr(&BinOp,
                                 scaleExpr(addExpr(*expr, constExpr(1), width),
                                           -1, width));
      if (trailingZeros > 0)
        return congruenceAssignmentForExpr(&BinOp, trailingZeros, *expr);
      if (trailingOnes > 0) {
        AffineExpr lowComplement =
            scaleExpr(addExpr(*expr, constExpr(1), width), -1, width);
        return congruenceAssignmentForExpr(&BinOp, trailingOnes, lowComplement);
      }
      break;
    }
    default:
      break;
    }
    return buildBitwisePartialRelation(BinOp);
  }

  Relation buildBitwisePartialRelation(const llvm::BinaryOperator &BinOp) const {
    auto lhsExpr = affineExprForValue(BinOp.getOperand(0));
    auto rhsExpr = affineExprForValue(BinOp.getOperand(1));
    if (!lhsExpr || !rhsExpr)
      return D::makeForget(&BinOp);

    unsigned width = BinOp.getType()->getIntegerBitWidth();
    PartialConstant lhs = partialConstantForExpr(*lhsExpr, width);
    PartialConstant rhs = partialConstantForExpr(*rhsExpr, width);
    unsigned knownBits = std::min(lhs.bits, rhs.bits);
    llvm::APInt knownValue(width, 0);
    switch (BinOp.getOpcode()) {
    case llvm::Instruction::And:
      knownValue = lhs.value & rhs.value;
      break;
    case llvm::Instruction::Or:
      knownValue = lhs.value | rhs.value;
      break;
    case llvm::Instruction::Xor:
      knownValue = lhs.value ^ rhs.value;
      break;
    default:
      return D::makeForget(&BinOp);
    }

    if (knownBits >= width) {
      return D::makeAffineAssignment(&BinOp, knownValue.getSExtValue(), {});
    }

    auto tryUseExtraKnownBits =
        [&](const PartialConstant &more, const PartialConstant &less,
            const AffineExpr &lessExpr,
            llvm::Instruction::BinaryOps opcode) -> std::optional<Relation> {
      if (more.bits <= knownBits)
        return std::nullopt;

      unsigned extraBits = more.bits - knownBits;
      llvm::APInt middle =
          more.value.lshr(knownBits) &
          llvm::APInt::getLowBitsSet(width, extraBits);
      bool lowMiddleBitIsOne = middle[0];
      unsigned runBits =
          lowMiddleBitIsOne ? countTrailingOnes(middle)
                            : middle.countTrailingZeros();
      runBits = std::min(runBits, extraBits);
      if (runBits == 0)
        return std::nullopt;

      unsigned modulusBits = knownBits + runBits;
      llvm::APInt lowResult = lowBitsValue(knownValue, knownBits);
      if (opcode == llvm::Instruction::And) {
        if (!lowMiddleBitIsOne)
          return congruenceAssignmentForConstant(&BinOp, modulusBits, lowResult);
        AffineExpr expr = addConstant(
            lessExpr, lowResult.getSExtValue() - lowBitsValue(less.value, knownBits).getSExtValue(),
            width);
        return congruenceAssignmentForExpr(&BinOp, modulusBits, expr);
      }
      if (opcode == llvm::Instruction::Or) {
        llvm::APInt middleMask =
            llvm::APInt::getLowBitsSet(width, modulusBits) ^
            llvm::APInt::getLowBitsSet(width, knownBits);
        if (lowMiddleBitIsOne) {
          llvm::APInt constant = lowResult | middleMask;
          return congruenceAssignmentForConstant(&BinOp, modulusBits, constant);
        }
        AffineExpr expr = addConstant(
            lessExpr, lowResult.getSExtValue() - lowBitsValue(less.value, knownBits).getSExtValue(),
            width);
        return congruenceAssignmentForExpr(&BinOp, modulusBits, expr);
      }
      if (opcode == llvm::Instruction::Xor) {
        if (!lowMiddleBitIsOne) {
          AffineExpr expr = addConstant(
              lessExpr,
              lowResult.getSExtValue() - lowBitsValue(less.value, knownBits).getSExtValue(),
              width);
          return congruenceAssignmentForExpr(&BinOp, modulusBits, expr);
        }
        AffineExpr expr = addConstant(
            scaleExpr(lessExpr, -1, width),
            lowBitsValue(less.value, knownBits).getSExtValue() +
                lowResult.getSExtValue() -
                llvm::APInt(width, 1).shl(knownBits).getSExtValue(),
            width);
        return congruenceAssignmentForExpr(&BinOp, modulusBits, expr);
      }
      return std::nullopt;
    };

    auto opcode = static_cast<llvm::Instruction::BinaryOps>(BinOp.getOpcode());
    if (auto relation = tryUseExtraKnownBits(lhs, rhs, *rhsExpr, opcode))
      return *relation;
    if (auto relation = tryUseExtraKnownBits(rhs, lhs, *lhsExpr, opcode))
      return *relation;

    if (knownBits == 0)
      return D::makeForget(&BinOp);
    return congruenceAssignmentForConstant(&BinOp, knownBits, knownValue);
  }

  Relation buildRemainderRelation(const llvm::BinaryOperator &BinOp) const {
    auto *Divisor = llvm::dyn_cast<llvm::ConstantInt>(BinOp.getOperand(1));
    auto expr = affineExprForValue(BinOp.getOperand(0));
    if (!Divisor || !expr)
      return D::makeForget(&BinOp);

    llvm::APInt divisor = Divisor->getValue();
    if (divisor.isZero())
      return D::makeForget(&BinOp);
    if (BinOp.getOpcode() == llvm::Instruction::SRem && divisor.isNegative())
      divisor = -divisor;

    unsigned rank = twoAdicRank(divisor);
    if (rank == 0)
      return D::makeForget(&BinOp);
    if (rank >= divisor.getBitWidth())
      return D::makeAffineAssignment(&BinOp, 0, {});
    return congruenceAssignmentForExpr(&BinOp, rank, *expr);
  }

  Relation buildBinaryRelation(const llvm::BinaryOperator &BinOp) const {
    if (auto expr = affineExprForValue(&BinOp))
      return assignmentForExpr(&BinOp, *expr);

    auto *L = BinOp.getOperand(0);
    auto *R = BinOp.getOperand(1);
    const unsigned width = D::bitWidthOf(&BinOp);
    int64_t lhsConst = 0, rhsConst = 0;
    auto *LC = llvm::dyn_cast<llvm::ConstantInt>(L);
    auto *RC = llvm::dyn_cast<llvm::ConstantInt>(R);
    if (LC && RC) {
      llvm::APInt lhsValue = LC->getValue();
      llvm::APInt rhsValue = RC->getValue();
      switch (BinOp.getOpcode()) {
      case llvm::Instruction::And:
        return D::makeAffineAssignment(&BinOp, (lhsValue & rhsValue).getSExtValue(),
                                       {});
      case llvm::Instruction::Or:
        return D::makeAffineAssignment(&BinOp, (lhsValue | rhsValue).getSExtValue(),
                                       {});
      case llvm::Instruction::Xor:
        return D::makeAffineAssignment(
            &BinOp, (lhsValue ^ rhsValue).getSExtValue(), {});
      case llvm::Instruction::LShr:
        return D::makeAffineAssignment(
            &BinOp, lhsValue.lshr(rhsValue.getLimitedValue()).getSExtValue(), {});
      case llvm::Instruction::AShr:
        return D::makeAffineAssignment(
            &BinOp, lhsValue.ashr(rhsValue.getLimitedValue()).getSExtValue(), {});
      case llvm::Instruction::UDiv:
        if (!rhsValue.isZero())
          return D::makeAffineAssignment(
              &BinOp, lhsValue.udiv(rhsValue).getSExtValue(), {});
        break;
      case llvm::Instruction::SDiv:
        if (!rhsValue.isZero())
          return D::makeAffineAssignment(
              &BinOp, lhsValue.sdiv(rhsValue).getSExtValue(), {});
        break;
      case llvm::Instruction::URem:
        if (!rhsValue.isZero())
          return D::makeAffineAssignment(
              &BinOp, lhsValue.urem(rhsValue).getSExtValue(), {});
        break;
      case llvm::Instruction::SRem:
        if (!rhsValue.isZero())
          return D::makeAffineAssignment(
              &BinOp, lhsValue.srem(rhsValue).getSExtValue(), {});
        break;
      default:
        break;
      }
    }
    switch (BinOp.getOpcode()) {
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
      if (LC || RC) {
        Relation partial = buildBitwisePartialRelation(BinOp);
        if (!D::equal(partial, D::makeForget(&BinOp)))
          return partial;
        return buildBitwiseMaskRelation(BinOp);
      }
      return buildBitwisePartialRelation(BinOp);
    case llvm::Instruction::LShr:
      if (auto *Shift = llvm::dyn_cast<llvm::ConstantInt>(R)) {
        auto lhsExpr = affineExprForValue(L);
        if (!lhsExpr || Shift->getValue().uge(width))
          return D::makeForget(&BinOp);
        if (auto quotient = exactQuotientByPowerOfTwo(
                *lhsExpr, width, Shift->getZExtValue())) {
          return assignmentForExpr(&BinOp, *quotient);
        }
      }
      return D::makeForget(&BinOp);
    case llvm::Instruction::UDiv:
      if (auto *Divisor = llvm::dyn_cast<llvm::ConstantInt>(R)) {
        auto lhsExpr = affineExprForValue(L);
        if (!lhsExpr || Divisor->isZero())
          return D::makeForget(&BinOp);
        if (auto log = exactPowerOfTwoLog(Divisor->getValue())) {
          if (auto quotient =
                  exactQuotientByPowerOfTwo(*lhsExpr, width, *log)) {
            return assignmentForExpr(&BinOp, *quotient);
          }
        }
      }
      return D::makeForget(&BinOp);
    case llvm::Instruction::URem:
    case llvm::Instruction::SRem:
      return buildRemainderRelation(BinOp);
    default:
      break;
    }

    switch (BinOp.getOpcode()) {
    case llvm::Instruction::Add:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst + rhsConst, width), {});
      if (getConstantIntValue(L, lhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(lhsConst, width),
                                       {{R, 1}});
      if (getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(rhsConst, width),
                                       {{L, 1}});
      if (D::isTrackedValue(L) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0, {{L, 1}, {R, 1}});
      return D::makeForget(&BinOp);
    case llvm::Instruction::Sub:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst - rhsConst, width), {});
      if (getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(-rhsConst, width),
                                       {{L, 1}});
      if (D::isTrackedValue(L) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0, {{L, 1}, {R, -1}});
      return D::makeForget(&BinOp);
    case llvm::Instruction::Mul:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst * rhsConst, width), {});
      if (getConstantIntValue(L, lhsConst) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0,
                                       {{R, wrapToBitWidth(lhsConst, width)}});
      if (getConstantIntValue(R, rhsConst) && D::isTrackedValue(L))
        return D::makeAffineAssignment(&BinOp, 0,
                                       {{L, wrapToBitWidth(rhsConst, width)}});
      return D::makeForget(&BinOp);
    default:
      return D::makeForget(&BinOp);
    }
  }
};

} // namespace

AffineState
materializeAffineExpressions(const AffineRelationDomain::value_type &relation) {
  return materializeAffineExpressionsImpl(relation);
}

InterAffineEqualities::Result
InterAffineEqualities::run(llvm::Module &M, bool verbose,
                           LinearStrategy linearStrategy,
                           IndirectCallResolutionMode callResolutionMode) {
  AffineRelationAnalysis analysis(M);
  auto engineResult =
      InterEngine<AffineRelationDomain, AffineRelationAnalysis>::run(
          M, analysis, verbose, linearStrategy, callResolutionMode);
  return analysis.buildResult(engineResult);
}

} // namespace npa
