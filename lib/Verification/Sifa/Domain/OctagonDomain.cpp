//===-- Verification/Sifa/Domain/OctagonDomain.cpp ------------------------===//
//
// Instruction-level block transfer for Sifa Octagon domain.
// Applies sound over-approximating transfer: copy/constant/affine assignments
// update octagon constraints; non-linear ops and memory havoc the result.
// When alias analysis is set, Load/Store use region-based memory (IKOS/CLAM style).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Domain/OctagonDomain.h"
#include "Verification/Sifa/RegionMemory.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <unordered_map>

using namespace lotus::sifa;

namespace {

/// Add variable \p v to state with no constraints (top for that variable).
OctagonState addVarUnconstrained(const OctagonState &s, const llvm::Value *v) {
  auto varToIndex = s.varToIndex();
  if (varToIndex.count(v)) return s;
  const std::size_t n = varToIndex.size();
  std::unordered_map<const llvm::Value *, std::size_t> newVarToIndex(varToIndex);
  newVarToIndex[v] = n;

  OctagonMatrix newMat(n + 1);
  const OctagonMatrix &old = s.matrix();
  for (std::size_t i = 0; i < old.dim(); ++i)
    for (std::size_t j = 0; j < old.dim(); ++j) {
      auto c = old.get(i, j);
      if (c) newMat.set(i, j, *c);
    }
  return OctagonState(std::move(newVarToIndex), std::move(newMat), s.isBottom());
}

/// Get index of \p v in state, or None if not present.
llvm::Optional<std::size_t> getVarIndex(const OctagonState &s,
                                        const llvm::Value *v) {
  auto it = s.varToIndex().find(v);
  if (it == s.varToIndex().end()) return llvm::None;
  return it->second;
}

/// Assign res = src (both must be in state). Octagon: res - src ≤ 0 and src - res ≤ 0.
OctagonState assignCopy(const OctagonState &s, const llvm::Value *res,
                        const llvm::Value *src) {
  auto ri = getVarIndex(s, res);
  auto si = getVarIndex(s, src);
  if (!ri || !si) return s;
  std::size_t r = *ri, srcIdx = *si;
  OctagonMatrix m = s.matrix();
  m.set(2 * r, 2 * srcIdx + 1, 0);
  m.set(2 * srcIdx, 2 * r + 1, 0);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop()) return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Assign res = c (constant). Octagon: 2*res ≤ 2c and -2*res ≤ -2c.
OctagonState assignConstant(const OctagonState &s, const llvm::Value *res,
                            int64_t c) {
  auto ri = getVarIndex(s, res);
  if (!ri) return s;
  std::size_t r = *ri;
  int64_t twoC;
  if (__builtin_mul_overflow(c, 2, &twoC)) return s;
  OctagonMatrix m = s.matrix();
  m.set(2 * r, 2 * r + 1, twoC);
  m.set(2 * r + 1, 2 * r, -twoC);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop()) return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Assign res = src + k (affine). Octagon: res - src ≤ k and src - res ≤ -k.
OctagonState assignAffine(const OctagonState &s, const llvm::Value *res,
                          const llvm::Value *src, int64_t k) {
  auto ri = getVarIndex(s, res);
  auto si = getVarIndex(s, src);
  if (!ri || !si) return s;
  std::size_t r = *ri, srcIdx = *si;
  OctagonMatrix m = s.matrix();
  m.set(2 * r, 2 * srcIdx + 1, k);
  m.set(2 * srcIdx, 2 * r + 1, -k);
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop()) return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Havoc variable \p v: relax all constraints involving v (sound over-approximation).
OctagonState havocVar(const OctagonState &s, const llvm::Value *v) {
  auto it = s.varToIndex().find(v);
  if (it == s.varToIndex().end()) return s;
  OctagonMatrix m = s.matrix().relaxVar(it->second);
  if (m.hasNegativeSelfLoop()) return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

/// Get constant from \p V if ConstantInt (fits in 64 bits), else None.
llvm::Optional<int64_t> getConstant(const llvm::Value *V) {
  const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
  if (!C || C->getBitWidth() > 64) return llvm::None;
  return C->getSExtValue();
}

/// Constrain result variable to interval [lo, hi]. Point => assignConstant; top => havoc.
OctagonState assignInterval(const OctagonState &s, const llvm::Value *res,
                            const Interval &interval) {
  auto ri = getVarIndex(s, res);
  if (!ri) return s;
  std::size_t r = *ri;
  if (interval.isBottom()) return OctagonState(true);
  if (interval.isPoint() && interval.lo) return assignConstant(s, res, *interval.lo);
  if (interval.isTop()) return havocVar(s, res);
  OctagonMatrix m = s.matrix();
  if (interval.hi) {
    int64_t twoHi;
    if (__builtin_mul_overflow(*interval.hi, 2, &twoHi)) return havocVar(s, res);
    m.set(2 * r + 1, 2 * r, twoHi);
  }
  if (interval.lo) {
    int64_t twoLo;
    if (__builtin_mul_overflow(*interval.lo, 2, &twoLo)) return havocVar(s, res);
    m.set(2 * r, 2 * r + 1, -twoLo);
  }
  m = m.strongClosure();
  if (m.hasNegativeSelfLoop()) return OctagonState(true);
  return OctagonState(s.varToIndex(), std::move(m), false);
}

} // namespace

OctagonState OctagonDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                               const OctagonState &in) const {
  if (in.isBottom()) return in;
  OctagonState out = in;

  lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
  std::vector<const llvm::Value *> regions;
  std::vector<const llvm::Value *> resolved;
  if (AA)
    regions = getRegionsForFunction(*bb->getParent());

  for (llvm::Instruction &I : *bb) {
    if (I.isTerminator()) break;
    if (I.getType()->isVoidTy()) {
      if (AA && llvm::isa<llvm::StoreInst>(&I)) {
        auto *SI = llvm::cast<llvm::StoreInst>(&I);
        const llvm::Value *ptr = SI->getPointerOperand();
        Interval val = Interval::top(); // Conservative: don't extract interval from octagon yet
        resolvePointerToRegions(AA, ptr, regions, resolved);
        for (const llvm::Value *r : resolved) {
          auto cur = out.getMemory(r);
          out.setMemory(r, cur ? cur->join(val) : val);
        }
      }
      continue;
    }
    if (!I.getType()->isIntegerTy() && !I.getType()->isPointerTy()) continue;

    const llvm::Value *res = &I;
    if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
      out = addVarUnconstrained(out, res);
      out.setMemory(&I, Interval::top());
      continue;
    }
    if (AA && llvm::isa<llvm::LoadInst>(&I) && I.getType()->isIntegerTy()) {
      const llvm::Value *ptr = llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
      resolvePointerToRegions(AA, ptr, regions, resolved);
      Interval loadVal = Interval::bottom();
      for (const llvm::Value *r : resolved) {
        auto cur = out.getMemory(r);
        Interval ir = cur ? *cur : Interval::top();
        loadVal = loadVal.isBottom() ? ir : loadVal.join(ir);
      }
      if (loadVal.isBottom()) loadVal = Interval::top();
      out = addVarUnconstrained(out, res);
      out = assignInterval(out, res, loadVal);
      continue;
    }
    if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      continue;
    }
    switch (I.getOpcode()) {
    case llvm::Instruction::PHI:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    case llvm::Instruction::Select: {
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    }
    case llvm::Instruction::Add: {
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      out = addVarUnconstrained(out, res);
      if (c0 && c1) {
        int64_t sum;
        if (__builtin_add_overflow(*c0, *c1, &sum))
          out = havocVar(out, res);
        else
          out = assignConstant(out, res, sum);
      } else if (c1 && *c1 == 0) {
        out = assignCopy(out, res, I.getOperand(0));
      } else if (c1) {
        out = assignAffine(out, res, I.getOperand(0), *c1);
      } else if (c0 && *c0 == 0) {
        out = assignCopy(out, res, I.getOperand(1));
      } else if (c0) {
        out = assignAffine(out, res, I.getOperand(1), *c0);
      } else {
        out = havocVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Sub: {
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      out = addVarUnconstrained(out, res);
      if (c0 && c1) {
        int64_t diff;
        if (__builtin_sub_overflow(*c0, *c1, &diff))
          out = havocVar(out, res);
        else
          out = assignConstant(out, res, diff);
      } else if (c1 && *c1 == 0) {
        out = assignCopy(out, res, I.getOperand(0));
      } else if (c1) {
        out = assignAffine(out, res, I.getOperand(0), -(*c1));
      } else if (c0) {
        out = havocVar(out, res);
      } else {
        out = havocVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast: {
      out = addVarUnconstrained(out, res);
      if (auto c = getConstant(I.getOperand(0)))
        out = assignConstant(out, res, *c);
      else
        out = assignCopy(out, res, I.getOperand(0));
      break;
    }
    case llvm::Instruction::ICmp:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    default:
      out = addVarUnconstrained(out, res);
      out = havocVar(out, res);
      break;
    }
  }
  return out;
}

OctagonState OctagonDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                                const OctagonState &in) const {
  if (in.isBottom()) return in;
  OctagonState out = in;
  // memory_ already copied from in
  for (llvm::Instruction &I : *bb) {
    if (I.isTerminator()) break;
    if (I.getType()->isVoidTy()) continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
      out = addVarUnconstrained(out, &I);
  }
  return out;
}

OctagonState OctagonDomain::post(const Transition &t,
                                 const OctagonState &in) const {
  if (in.isBottom()) return in;
  if (t.kind == TransitionKind::Marker) return in;
  if (t.kind == TransitionKind::ReturnSummary) return in;
  if (t.kind != TransitionKind::Edge || !t.source) return in;
  if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source))
    return applyBlockWiseHavoc(t.source, in);
  return applyBlockTransfer(t.source, in);
}

void OctagonState::print(llvm::raw_ostream &out) const {
  if (isBottom_) {
    out << "  (bottom)\n";
    return;
  }
  out << "  variables: " << varToIndex_.size();
  if (!memory_.empty())
    out << ", memory regions: " << memory_.size();
  out << "\n";
}
