//===-- Verification/Sifa/Domain/ExplicitValueDomain.h --------------------===//
//
// Domain of explicit variable valuations (Ultimate ExplicitValueDomain-aligned).
//
// Ultimate's ExplicitValueDomain(SymbolicTools, maxDisjuncts) represents
// states as DNF of variable = constant; join limits disjuncts. In lotus we
// use a single map Value* -> optional constant (constant propagation style);
// join: same constant => keep, different => top (drop binding).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

#include "llvm/ADT/Optional.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include <unordered_map>

namespace lotus {
namespace sifa {

/// Single explicit value: constant or top (no info). Ultimate INonrelationalValue for constants.
struct ExplicitValue {
  llvm::Optional<int64_t> value;

  static ExplicitValue top() { return {llvm::None}; }
  static ExplicitValue constant(int64_t c) { return {c}; }
  bool isTop() const { return !value.hasValue(); }
  bool isBottom() const { return false; } // no explicit bottom in constant domain

  ExplicitValue join(const ExplicitValue &rhs) const {
    if (isTop() || rhs.isTop()) return top();
    if (value.getValue() == rhs.value.getValue()) return *this;
    return top();
  }
  ExplicitValue widen(const ExplicitValue &rhs) const { return join(rhs); }
};

/// State: map Value* -> optional constant. Join: same var same constant => keep; else => top.
struct ExplicitValueState {
  std::unordered_map<const llvm::Value *, ExplicitValue> map_;

  bool isBottom() const { return false; }
  llvm::Optional<ExplicitValue> get(const llvm::Value *v) const {
    auto it = map_.find(v);
    if (it == map_.end()) return llvm::Optional<ExplicitValue>(ExplicitValue::top());
    return it->second;
  }
  void set(const llvm::Value *v, ExplicitValue val) {
    if (val.isTop()) map_.erase(v);
    else map_[v] = std::move(val);
  }
};

/// Explicit value domain (constant propagation style). post(Edge): applies block transfer (constant propagation).
/// When BlockTransferPolicy marks a block as block-wise, post(Edge) uses applyBlockWiseHavoc (no new constants).
class ExplicitValueDomain final : public AbstractDomain<Transition, ExplicitValueState> {
public:
  using State = ExplicitValueState;

  ExplicitValueDomain() = default;
  explicit ExplicitValueDomain(const BlockTransferPolicy *policy) : blockTransferPolicy_(policy) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const { return blockTransferPolicy_; }

  State top() const override { return State{}; }
  State bottom() const override { return State{}; }
  bool isBottom(const State &s) const override { return s.isBottom(); }

  bool leq(const State &a, const State &b) const override {
    for (const auto &kv : a.map_) {
      auto ob = b.get(kv.first);
      if (!ob.hasValue() || ob.getValue().isTop()) continue;
      if (kv.second.isTop()) return false;
      if (kv.second.value != ob.getValue().value) return false;
    }
    return true;
  }
  State join(const State &a, const State &b) const override {
    State r;
    for (const auto &kv : a.map_) {
      auto ob = b.get(kv.first);
      ExplicitValue j = kv.second;
      if (ob.hasValue()) j = j.join(ob.getValue());
      if (!j.isTop()) r.set(kv.first, j);
    }
    for (const auto &kv : b.map_) {
      if (r.map_.find(kv.first) != r.map_.end()) continue;
      auto oa = a.get(kv.first);
      ExplicitValue j = kv.second;
      if (oa.hasValue()) j = j.join(oa.getValue());
      if (!j.isTop()) r.set(kv.first, j);
    }
    return r;
  }
  State widen(const State &prev, const State &next) const override {
    return join(prev, next);
  }

  /// Block-wise fast path: do not track constants for this block (sound, less precise).
  State applyBlockWiseHavoc(llvm::BasicBlock *bb, const State &in) const {
    (void)bb;
    return in; // Leave state unchanged; values defined in bb are unknown (top).
  }
  State post(const Transition &t, const State &in) const override {
    if (t.kind != TransitionKind::Edge || !t.source) return in;
    if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source))
      return applyBlockWiseHavoc(t.source, in);
    State out = in;
    for (llvm::Instruction &I : *t.source) {
      if (I.isTerminator()) break;
      if (I.getType()->isVoidTy()) continue;
      ExplicitValue res = transferInstruction(I, out);
      if (!res.isTop()) out.set(&I, res);
    }
    return out;
  }
  State postCall(const State &callerState) const override { return callerState; }
  State postReturn(const State &callerState, const State &calleeSummary) const override {
    return join(callerState, calleeSummary);
  }

private:
  static ExplicitValue getConst(const State &s, const llvm::Value *V) {
    if (!V) return ExplicitValue::top();
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
      if (C->getBitWidth() > 64) return ExplicitValue::top();
      return ExplicitValue::constant(C->getSExtValue());
    }
    auto it = s.map_.find(V);
    if (it != s.map_.end() && !it->second.isTop()) return it->second;
    return ExplicitValue::top();
  }
  static ExplicitValue transferInstruction(const llvm::Instruction &I,
                                          const State &state) {
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(&I)) {
      if (C->getBitWidth() > 64) return ExplicitValue::top();
      return ExplicitValue::constant(C->getSExtValue());
    }
    switch (I.getOpcode()) {
    case llvm::Instruction::Add: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value + *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Sub: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value - *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Mul: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop())
        return ExplicitValue::constant(*L.value * *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop() && *R.value != 0)
        return ExplicitValue::constant(*L.value / *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem: {
      auto L = getConst(state, I.getOperand(0));
      auto R = getConst(state, I.getOperand(1));
      if (!L.isTop() && !R.isTop() && *R.value != 0)
        return ExplicitValue::constant(*L.value % *R.value);
      return ExplicitValue::top();
    }
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast:
      return getConst(state, I.getOperand(0));
    case llvm::Instruction::PHI: {
      auto *Phi = llvm::cast<llvm::PHINode>(&I);
      ExplicitValue acc = getConst(state, Phi->getIncomingValue(0));
      for (unsigned i = 1, e = Phi->getNumIncomingValues(); i < e; ++i)
        acc = acc.join(getConst(state, Phi->getIncomingValue(i)));
      return acc;
    }
    case llvm::Instruction::Select: {
      auto T = getConst(state, I.getOperand(1));
      auto F = getConst(state, I.getOperand(2));
      return T.join(F);
    }
    default:
      return ExplicitValue::top();
    }
  }

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H
