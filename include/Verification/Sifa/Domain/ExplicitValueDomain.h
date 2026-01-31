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

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

#include "llvm/IR/Value.h"

#include <optional>
#include <unordered_map>

namespace lotus {
namespace sifa {

/// Single explicit value: constant or top (no info). Ultimate INonrelationalValue for constants.
struct ExplicitValue {
  std::optional<int64_t> value;

  static ExplicitValue top() { return {std::nullopt}; }
  static ExplicitValue constant(int64_t c) { return {c}; }
  bool isTop() const { return !value.has_value(); }
  bool isBottom() const { return false; } // no explicit bottom in constant domain

  ExplicitValue join(const ExplicitValue &rhs) const {
    if (isTop() || rhs.isTop()) return top();
    if (*value == *rhs.value) return *this;
    return top();
  }
  ExplicitValue widen(const ExplicitValue &rhs) const { return join(rhs); }
};

/// State: map Value* -> optional constant. Join: same var same constant => keep; else => top.
struct ExplicitValueState {
  std::unordered_map<const llvm::Value *, ExplicitValue> map_;

  bool isBottom() const { return false; }
  std::optional<ExplicitValue> get(const llvm::Value *v) const {
    auto it = map_.find(v);
    if (it == map_.end()) return ExplicitValue::top();
    return it->second;
  }
  void set(const llvm::Value *v, ExplicitValue val) {
    if (val.isTop()) map_.erase(v);
    else map_[v] = std::move(val);
  }
};

/// Explicit value domain (constant propagation style). post(Edge): identity; full transfer can be added.
class ExplicitValueDomain final : public AbstractDomain<Transition, ExplicitValueState> {
public:
  using State = ExplicitValueState;

  State top() const override { return State{}; }
  State bottom() const override { return State{}; }
  bool isBottom(const State &s) const override { return s.isBottom(); }

  bool leq(const State &a, const State &b) const override {
    for (const auto &[v, va] : a.map_) {
      auto ob = b.get(v);
      if (!ob || ob->isTop()) continue;
      if (va.isTop()) return false;
      if (va.value != ob->value) return false;
    }
    return true;
  }
  State join(const State &a, const State &b) const override {
    State r;
    for (const auto &[v, va] : a.map_) {
      auto ob = b.get(v);
      ExplicitValue j = va;
      if (ob) j = j.join(*ob);
      if (!j.isTop()) r.set(v, j);
    }
    for (const auto &[v, vb] : b.map_) {
      if (r.get(v)) continue;
      auto oa = a.get(v);
      ExplicitValue j = vb;
      if (oa) j = j.join(*oa);
      if (!j.isTop()) r.set(v, j);
    }
    return r;
  }
  State widen(const State &prev, const State &next) const override {
    return join(prev, next);
  }

  State post(const Transition &t, const State &in) const override {
    (void)t;
    return in;
  }
  State postCall(const State &callerState) const override { return callerState; }
  State postReturn(const State &callerState, const State &calleeSummary) const override {
    return join(callerState, calleeSummary);
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_EXPLICITVALUEDOMAIN_H
