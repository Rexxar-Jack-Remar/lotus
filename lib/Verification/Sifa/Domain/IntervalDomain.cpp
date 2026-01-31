//===-- Verification/Sifa/Domain/IntervalDomain.cpp -----------------------===//
//
// Instruction-level block transfer for Sifa Interval domain.
// Applies sound over-approximating transfer for each LLVM instruction in a
// basic block so that post(Edge) models real program semantics.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Domain/IntervalDomain.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

using namespace lotus::sifa;

namespace {

/// Return the interval for \p V from \p state, or Interval::top() if unknown.
Interval getInterval(const IntervalState &state, const llvm::Value *V) {
  if (!V) return Interval::top();
  auto opt = state.get(V);
  if (opt.hasValue()) return opt.getValue();
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    if (C->getBitWidth() > 64) return Interval::top();
    int64_t val = C->getSExtValue();
    return Interval::point(val);
  }
  return Interval::top();
}

/// Restrict interval to signed range of \p bits (e.g. i32 -> [-2^31, 2^31-1]).
Interval restrictToSigned(const Interval &i, unsigned bits) {
  if (i.isBottom()) return i;
  if (bits >= 64) return i;
  int64_t minVal = -(1LL << (bits - 1));
  int64_t maxVal = (1LL << (bits - 1)) - 1;
  llvm::Optional<int64_t> lo = i.lo.hasValue()
                                   ? std::max(*i.lo, minVal)
                                   : llvm::Optional<int64_t>(minVal);
  llvm::Optional<int64_t> hi = i.hi.hasValue()
                                   ? std::min(*i.hi, maxVal)
                                   : llvm::Optional<int64_t>(maxVal);
  if (lo && hi && *lo > *hi) return Interval::bottom();
  return Interval{lo, hi, false};
}

/// Restrict interval to unsigned range [0, 2^bits - 1].
Interval restrictToUnsigned(const Interval &i, unsigned bits) {
  if (i.isBottom()) return i;
  if (bits >= 64) return i;
  int64_t maxVal = (bits == 64) ? INT64_MAX : ((1LL << bits) - 1);
  llvm::Optional<int64_t> lo = i.lo.hasValue()
                                   ? std::max(*i.lo, 0LL)
                                   : llvm::Optional<int64_t>(0);
  llvm::Optional<int64_t> hi = i.hi.hasValue()
                                   ? std::min(*i.hi, maxVal)
                                   : llvm::Optional<int64_t>(maxVal);
  if (lo && hi && *lo > *hi) return Interval::bottom();
  return Interval{lo, hi, false};
}

unsigned getBitWidth(const llvm::Value *V) {
  if (!V) return 64;
  auto *Ty = V->getType();
  if (Ty->isIntegerTy()) return Ty->getIntegerBitWidth();
  return 64;
}

/// Transfer for a single instruction: compute result interval from operands.
Interval transferInstruction(const llvm::Instruction &I,
                              const IntervalState &state) {
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(&I)) {
    if (C->getBitWidth() > 64) return Interval::top();
    return Interval::point(C->getSExtValue());
  }

  switch (I.getOpcode()) {
  case llvm::Instruction::Add: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.add(R), getBitWidth(&I));
  }
  case llvm::Instruction::Sub: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.subtract(R), getBitWidth(&I));
  }
  case llvm::Instruction::Mul: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.multiply(R), getBitWidth(&I));
  }
  case llvm::Instruction::SDiv: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    return restrictToSigned(L.divide(R), getBitWidth(&I));
  }
  case llvm::Instruction::UDiv: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero()) return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi) return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    uint64_t u00 = static_cast<uint64_t>(*L.lo) / static_cast<uint64_t>(*R.lo);
    uint64_t u01 = static_cast<uint64_t>(*L.lo) / static_cast<uint64_t>(*R.hi);
    uint64_t u10 = static_cast<uint64_t>(*L.hi) / static_cast<uint64_t>(*R.lo);
    uint64_t u11 = static_cast<uint64_t>(*L.hi) / static_cast<uint64_t>(*R.hi);
    int64_t lo = static_cast<int64_t>(std::min({u00, u01, u10, u11}));
    int64_t hi = static_cast<int64_t>(std::max({u00, u01, u10, u11}));
    return restrictToUnsigned(Interval{lo, hi, false}, getBitWidth(&I));
  }
  case llvm::Instruction::SRem: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero()) return restrictToSigned(Interval::top(), getBitWidth(&I));
    if (!R.lo || !R.hi) return restrictToSigned(Interval::top(), getBitWidth(&I));
    int64_t rLo = std::abs(*R.lo), rHi = std::abs(*R.hi);
    int64_t bound = std::max(rLo, rHi) - 1;
    return restrictToSigned(Interval{-bound, bound, false}, getBitWidth(&I));
  }
  case llvm::Instruction::URem: {
    auto R = getInterval(state, I.getOperand(1));
    if (R.containsZero()) return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    unsigned w = getBitWidth(&I);
    int64_t maxVal = (w >= 64) ? INT64_MAX : ((1LL << w) - 1);
    return restrictToUnsigned(Interval{0, maxVal, false}, w);
  }
  case llvm::Instruction::Shl: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi) return restrictToSigned(Interval::top(), getBitWidth(&I));
    if (*R.hi < 0 || *R.lo > 63) return restrictToSigned(Interval::top(), getBitWidth(&I));
    int64_t shAmtLo = *R.lo, shAmtHi = std::min(*R.hi, 63LL);
    int64_t v00 = *L.lo << shAmtLo, v01 = *L.lo << shAmtHi;
    int64_t v10 = *L.hi << shAmtLo, v11 = *L.hi << shAmtHi;
    return restrictToSigned(
        Interval{std::min({v00, v01, v10, v11}), std::max({v00, v01, v10, v11}), false},
        getBitWidth(&I));
  }
  case llvm::Instruction::LShr: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi) return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    if (*R.hi < 0 || *R.lo > 63) return restrictToUnsigned(Interval::top(), getBitWidth(&I));
    unsigned w = getBitWidth(&I);
    uint64_t mask = (w >= 64) ? UINT64_MAX : ((1ULL << w) - 1);
    uint64_t uLo = static_cast<uint64_t>(*L.lo) & mask;
    uint64_t uHi = static_cast<uint64_t>(*L.hi) & mask;
    int64_t shAmt = std::min(*R.lo, 63LL);
    uint64_t rLo = uLo >> shAmt, rHi = uHi >> shAmt;
    return restrictToUnsigned(
        Interval{static_cast<int64_t>(std::min(rLo, rHi)), static_cast<int64_t>(std::max(rLo, rHi)), false},
        w);
  }
  case llvm::Instruction::AShr: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    if (!L.lo || !L.hi || !R.lo || !R.hi) return restrictToSigned(Interval::top(), getBitWidth(&I));
    if (*R.hi < 0 || *R.lo > 63) return restrictToSigned(Interval::top(), getBitWidth(&I));
    int64_t shAmt = std::min(*R.lo, 63LL);
    int64_t vLo = *L.lo >> shAmt, vHi = *L.hi >> shAmt;
    return restrictToSigned(Interval{std::min(vLo, vHi), std::max(vLo, vHi), false}, getBitWidth(&I));
  }
  case llvm::Instruction::And: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    unsigned w = getBitWidth(&I);
    if (w > 64) return Interval::top();
    int64_t maxVal = (w == 64) ? INT64_MAX : ((1LL << w) - 1);
    if (!L.lo || !L.hi || !R.lo || !R.hi)
      return restrictToUnsigned(Interval{0, maxVal, false}, w);
    int64_t lo = 0, hi = maxVal;
    if (*L.lo >= 0 && *L.hi >= 0 && *R.lo >= 0 && *R.hi >= 0) {
      uint64_t u = static_cast<uint64_t>(*L.hi) & static_cast<uint64_t>(*R.hi);
      hi = static_cast<int64_t>(std::min(u, static_cast<uint64_t>(maxVal)));
    }
    return restrictToUnsigned(Interval{lo, hi, false}, w);
  }
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    unsigned w = getBitWidth(&I);
    return restrictToUnsigned(Interval::top(), w);
  }
  case llvm::Instruction::Trunc: {
    auto Op = getInterval(state, I.getOperand(0));
    unsigned w = I.getType()->getIntegerBitWidth();
    return restrictToUnsigned(Op, w);
  }
  case llvm::Instruction::ZExt: {
    auto Op = getInterval(state, I.getOperand(0));
    unsigned wIn = I.getOperand(0)->getType()->getIntegerBitWidth();
    unsigned wOut = getBitWidth(&I);
    if (Op.isBottom()) return Interval::bottom();
    if (!Op.lo || !Op.hi) return restrictToUnsigned(Interval::top(), wOut);
    if (*Op.lo >= 0 && *Op.hi >= 0)
      return restrictToUnsigned(Interval{*Op.lo, *Op.hi, false}, wOut);
    int64_t maxIn = (wIn >= 64) ? INT64_MAX : ((1LL << wIn) - 1);
    return restrictToUnsigned(Interval{0, maxIn, false}, wOut);
  }
  case llvm::Instruction::SExt: {
    auto Op = getInterval(state, I.getOperand(0));
    return restrictToSigned(Op, getBitWidth(&I));
  }
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
    return restrictToSigned(getInterval(state, I.getOperand(0)), getBitWidth(&I));

  case llvm::Instruction::ICmp: {
    auto L = getInterval(state, I.getOperand(0));
    auto R = getInterval(state, I.getOperand(1));
    if (L.isBottom() || R.isBottom()) return Interval::bottom();
    auto *Cmp = llvm::cast<llvm::CmpInst>(&I);
    bool isSigned = Cmp->isSigned();
    switch (Cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      if (L.isPoint() && R.isPoint() && *L.lo == *R.lo) return Interval::point(1);
      if (!L.intersect(R).isBottom()) return Interval{0, 1, false};
      return Interval::point(0);
    case llvm::CmpInst::ICMP_NE:
      if (L.isPoint() && R.isPoint() && *L.lo == *R.lo) return Interval::point(0);
      if (!L.intersect(R).isBottom()) return Interval{0, 1, false};
      return Interval::point(1);
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_SLE:
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_UGE:
      return Interval{0, 1, false};
    default:
      return Interval::top();
    }
  }
  case llvm::Instruction::Select: {
    auto Cond = getInterval(state, I.getOperand(0));
    auto TrueVal = getInterval(state, I.getOperand(1));
    auto FalseVal = getInterval(state, I.getOperand(2));
    if (TrueVal.isBottom() && FalseVal.isBottom()) return Interval::bottom();
    if (Cond.containsZero() && Cond.isPoint() && Cond.lo && *Cond.lo != 0)
      return TrueVal;
    if (Cond.containsZero() && Cond.isPoint() && Cond.lo && *Cond.lo == 0)
      return FalseVal;
    return TrueVal.join(FalseVal);
  }
  case llvm::Instruction::PHI: {
    auto *Phi = llvm::cast<llvm::PHINode>(&I);
    Interval acc = Interval::bottom();
    for (unsigned i = 0, e = Phi->getNumIncomingValues(); i < e; ++i) {
      Interval in = getInterval(state, Phi->getIncomingValue(i));
      acc = acc.isBottom() ? in : acc.join(in);
    }
    return acc.isBottom() ? Interval::top() : acc;
  }
  case llvm::Instruction::Alloca:
  case llvm::Instruction::Load:
  case llvm::Instruction::GetElementPtr:
  case llvm::Instruction::Call:
  case llvm::Instruction::Invoke:
  default:
    return Interval::top();
  }
}

} // namespace

IntervalState IntervalDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                                 const IntervalState &in) const {
  if (in.isBottom()) return in;
  IntervalState out(false);
  for (const auto &kv : in.intervals())
    out.set(kv.first, kv.second);

  for (llvm::Instruction &I : *bb) {
    if (I.isTerminator()) break;
    if (I.getType()->isVoidTy()) continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy()) {
      Interval res = transferInstruction(I, out);
      out.set(&I, std::move(res));
    }
  }
  return out;
}

IntervalState IntervalDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                                 const IntervalState &in) const {
  if (in.isBottom()) return in;
  IntervalState out(false);
  for (const auto &kv : in.intervals())
    out.set(kv.first, kv.second);
  for (llvm::Instruction &I : *bb) {
    if (I.isTerminator()) break;
    if (I.getType()->isVoidTy()) continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
      out.set(&I, Interval::top());
  }
  return out;
}

IntervalState IntervalDomain::post(const Transition &t,
                                  const IntervalState &in) const {
  if (in.isBottom()) return in;
  if (t.kind == TransitionKind::Marker) return in;
  if (t.kind == TransitionKind::ReturnSummary) return in;
  if (t.kind != TransitionKind::Edge || !t.source) return in;
  if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source))
    return applyBlockWiseHavoc(t.source, in);
  return applyBlockTransfer(t.source, in);
}
