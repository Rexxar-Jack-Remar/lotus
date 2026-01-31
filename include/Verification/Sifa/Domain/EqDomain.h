//===-- Verification/Sifa/Domain/EqDomain.h -------------------------------===//
//
// Equality domain (ported from Ultimate Library-Sifa).
//
// Ultimate's EqDomain is StateBasedDomain<EqState> with EqConstraint<EqNode>.
// Lotus uses union-find over LLVM Value* for equality classes; join merges
// classes from both states; post(Edge) applies block transfer (copy/phi/select equality).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_EQDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_EQDOMAIN_H

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace sifa {

/// Equality state: union-find over Value* (Ultimate EqState wraps EqConstraint).
class EqState {
public:
  EqState() = default;
  explicit EqState(bool isBottom) : isBottom_(isBottom) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  /// Representative of \p v (path compression). If \p v not seen, returns v.
  const llvm::Value *find(const llvm::Value *v) const {
    if (!v) return v;
    auto it = parent_.find(v);
    if (it == parent_.end()) return v;
    if (it->second == v) return v;
    const llvm::Value *root = find(it->second);
    parent_[v] = root;
    return root;
  }

  /// Union-find link: make \p a and \p b equivalent.
  void unite(const llvm::Value *a, const llvm::Value *b) {
    if (!a || !b) return;
    const llvm::Value *ra = find(a);
    const llvm::Value *rb = find(b);
    if (ra == rb) return;
    ensure(ra);
    ensure(rb);
    parent_[ra] = rb;
  }

  /// Ensure \p v is in the map (self-loop).
  void ensure(const llvm::Value *v) {
    if (v && parent_.find(v) == parent_.end()) parent_[v] = v;
  }

  /// All keys (values that have been seen).
  std::unordered_set<const llvm::Value *> keys() const {
    std::unordered_set<const llvm::Value *> k;
    for (const auto &p : parent_) k.insert(p.first);
    return k;
  }

  EqState join(const EqState &other) const {
    if (isBottom_) return other;
    if (other.isBottom_) return *this;
    EqState out;
    for (const llvm::Value *v : keys()) out.ensure(v);
    for (const llvm::Value *v : other.keys()) out.ensure(v);
    for (const llvm::Value *v : keys()) out.unite(v, find(v));
    for (const llvm::Value *v : other.keys()) out.unite(v, other.find(v));
    return out;
  }

  EqState widen(const EqState &other) const { return join(other); }

  bool operator==(const EqState &o) const {
    if (isBottom_ != o.isBottom_) return false;
    auto k1 = keys(), k2 = o.keys();
    if (k1.size() != k2.size()) return false;
    for (const llvm::Value *v : k1) {
      if (!k2.count(v)) return false;
      if (find(v) != o.find(v)) return false;
    }
    return true;
  }

private:
  bool isBottom_ = false;
  mutable std::unordered_map<const llvm::Value *, const llvm::Value *> parent_; // path compression in find()
};

/// Equality domain implementing AbstractDomain<Transition, EqState>.
/// When BlockTransferPolicy marks a block as block-wise, post(Edge) uses
/// applyBlockWiseHavoc (ensure all defined values, no new equalities).
class EqDomain final : public AbstractDomain<Transition, EqState> {
public:
  EqDomain() = default;
  explicit EqDomain(const BlockTransferPolicy *policy) : blockTransferPolicy_(policy) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const { return blockTransferPolicy_; }

  EqState top() const override { return EqState(false); }
  EqState bottom() const override { return EqState(true); }
  bool isBottom(const EqState &s) const override { return s.isBottom(); }
  bool leq(const EqState &a, const EqState &b) const override {
    if (a.isBottom()) return true;
    if (b.isBottom()) return false;
    for (const llvm::Value *v : a.keys()) {
      if (!b.keys().count(v)) continue;
      if (a.find(v) != b.find(v)) return false;
    }
    return true;
  }
  EqState join(const EqState &a, const EqState &b) const override { return a.join(b); }
  EqState widen(const EqState &prev, const EqState &next) const override {
    return prev.widen(next);
  }
  EqState applyBlockWiseHavoc(llvm::BasicBlock *bb, const EqState &in) const {
    if (in.isBottom()) return in;
    EqState out = in;
    for (llvm::Instruction &I : *bb) {
      if (I.isTerminator()) break;
      if (I.getType()->isVoidTy()) continue;
      out.ensure(&I);
    }
    return out;
  }
  EqState post(const Transition &t, const EqState &in) const override {
    if (in.isBottom()) return in;
    if (t.kind != TransitionKind::Edge || !t.source) return in;
    if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source))
      return applyBlockWiseHavoc(t.source, in);
    EqState out = in;
    for (llvm::Instruction &I : *t.source) {
      if (I.isTerminator()) break;
      if (I.getType()->isVoidTy()) continue;
      if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
        for (unsigned i = 0, e = Phi->getNumIncomingValues(); i < e; ++i)
          out.unite(&I, Phi->getIncomingValue(i));
      } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
        out.unite(&I, Cast->getOperand(0));
      } else if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&I)) {
        out.unite(&I, Sel->getTrueValue());
        out.unite(&I, Sel->getFalseValue());
      } else {
        out.ensure(&I);
      }
    }
    return out;
  }

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_EQDOMAIN_H
