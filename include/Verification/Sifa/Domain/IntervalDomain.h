//===-- Verification/Sifa/Domain/IntervalDomain.h --------------------------===//
//
// Interval domain for Sifa (ported from Ultimate Library-Sifa).
//
// State: map from LLVM Value* to [lo, hi]. Join = interval hull, widen = standard.
// post(Edge): identity (full transfer over block instructions can be added).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

#include "llvm/IR/Value.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

namespace lotus {
namespace sifa {

/// Ultimate-aligned: SatisfyingInputs for satisfyEqual/satisfyLessOrEqual etc.
struct SatisfyingInputs {
  Interval lhs;
  Interval rhs;
  SatisfyingInputs() = default;
  explicit SatisfyingInputs(const Interval &lhsAndRhs) : lhs(lhsAndRhs), rhs(lhsAndRhs) {}
  SatisfyingInputs(const Interval &l, const Interval &r) : lhs(l), rhs(r) {}
  const Interval &getLhs() const { return lhs; }
  const Interval &getRhs() const { return rhs; }
  SatisfyingInputs swap() const { return SatisfyingInputs(rhs, lhs); }
};

/// Single interval [lo, hi]; unbounded when optional is nullopt (Ultimate-aligned).
/// Bottom = empty set; top = [-inf, +inf] (no bounds).
struct Interval {
  std::optional<int64_t> lo;
  std::optional<int64_t> hi;
  /// True iff this interval represents bottom (empty set). When false and both lo/hi nullopt, interval is top.
  bool isBottom_ = false;

  static Interval bottom() {
    Interval i;
    i.isBottom_ = true;
    return i;
  }
  static Interval top() { return {std::nullopt, std::nullopt, false}; }
  static Interval point(int64_t p) { return {p, p, false}; }
  bool isBottom() const { return isBottom_; }
  bool isTop() const { return !isBottom_ && !lo && !hi; }
  bool isUnbounded() const { return !lo || !hi; }

  /// Ultimate: hasLower/hasUpper — finite bound.
  bool hasLower() const { return lo.has_value(); }
  bool hasUpper() const { return hi.has_value(); }
  int64_t getLower() const { return lo.value_or(0); }
  int64_t getUpper() const { return hi.value_or(0); }
  bool isPoint() const { return lo && hi && *lo == *hi; }
  bool containsZero() const {
    if (isBottom_) return false;
    return (!lo || *lo <= 0) && (!hi || *hi >= 0);
  }

  /// Hull (join): [min(lo,rhs.lo), max(hi,rhs.hi)]. Bottom is identity for join.
  Interval join(const Interval &rhs) const {
    if (isBottom()) return rhs;
    if (rhs.isBottom()) return *this;
    Interval r;
    r.lo = (lo && rhs.lo) ? std::optional<int64_t>(std::min(*lo, *rhs.lo)) : std::nullopt;
    r.hi = (hi && rhs.hi) ? std::optional<int64_t>(std::max(*hi, *rhs.hi)) : std::nullopt;
    return r;
  }
  /// Standard widening (Ultimate-aligned). Bottom is identity.
  Interval widen(const Interval &rhs) const {
    if (isBottom()) return rhs;
    if (rhs.isBottom()) return *this;
    Interval r;
    r.lo = (lo && rhs.lo && *rhs.lo < *lo) ? std::nullopt : rhs.lo;
    r.hi = (hi && rhs.hi && *rhs.hi > *hi) ? std::nullopt : rhs.hi;
    return r;
  }
  Interval negate() const {
    if (isBottom()) return bottom();
    Interval r;
    r.lo = hi ? std::optional<int64_t>(-*hi) : std::nullopt;
    r.hi = lo ? std::optional<int64_t>(-*lo) : std::nullopt;
    return r;
  }
  Interval add(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    Interval r;
    r.lo = (lo && rhs.lo) ? std::optional<int64_t>(*lo + *rhs.lo) : std::nullopt;
    r.hi = (hi && rhs.hi) ? std::optional<int64_t>(*hi + *rhs.hi) : std::nullopt;
    return r;
  }
  Interval subtract(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    Interval r;
    r.lo = (lo && rhs.hi) ? std::optional<int64_t>(*lo - *rhs.hi) : std::nullopt;
    r.hi = (hi && rhs.lo) ? std::optional<int64_t>(*hi - *rhs.lo) : std::nullopt;
    return r;
  }
  /// Ultimate-aligned: [a,b]*[c,d] = [min(ac,ad,bc,bd), max(ac,ad,bc,bd)].
  Interval multiply(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!lo || !hi || !rhs.lo || !rhs.hi) return top();
    int64_t v00 = *lo * *rhs.lo, v01 = *lo * *rhs.hi, v10 = *hi * *rhs.lo, v11 = *hi * *rhs.hi;
    return {std::min({v00, v01, v10, v11}), std::max({v00, v01, v10, v11}), false};
  }
  /// Ultimate: divide; containsZero(rhs) => top.
  Interval divide(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (rhs.containsZero()) return top();
    if (!lo || !hi || !rhs.lo || !rhs.hi) return top();
    int64_t v00 = *lo / *rhs.lo, v01 = *lo / *rhs.hi, v10 = *hi / *rhs.lo, v11 = *hi / *rhs.hi;
    return {std::min({v00, v01, v10, v11}), std::max({v00, v01, v10, v11}), false};
  }
  /// Ultimate: intersect — [max(lo,rhs.lo), min(hi,rhs.hi)]; unbounded sides preserved.
  Interval intersect(const Interval &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    std::optional<int64_t> l = (lo && rhs.lo) ? std::max(*lo, *rhs.lo) : (lo ? lo : rhs.lo);
    std::optional<int64_t> h = (hi && rhs.hi) ? std::min(*hi, *rhs.hi) : (hi ? hi : rhs.hi);
    if (l && h && *l > *h) return bottom();
    Interval r;
    r.lo = l;
    r.hi = h;
    r.isBottom_ = false;
    return r;
  }
  /// Ultimate: complement — one half when half-unbounded; top when fully unbounded; bottom when empty.
  Interval complement() const {
    if (isBottom()) return top();
    if (!hasLower() && !hasUpper()) return bottom();
    if (hasLower() && !hasUpper()) return {std::nullopt, std::optional<int64_t>(*lo - 1), false};
    if (!hasLower() && hasUpper()) return {std::optional<int64_t>(*hi + 1), std::nullopt, false};
    return top();
  }
  SatisfyingInputs satisfyEqual(const Interval &rhs) const { return SatisfyingInputs(intersect(rhs)); }
  SatisfyingInputs satisfyDistinct(const Interval &rhs) const {
    if (isPoint() && rhs.isPoint() && *lo == *rhs.lo) return SatisfyingInputs(bottom());
    return SatisfyingInputs(*this, rhs);
  }
  SatisfyingInputs satisfyLessOrEqual(const Interval &rhs) const {
    return SatisfyingInputs(
        {lo, hi && rhs.hi ? std::min(*hi, *rhs.hi) : hi},
        {rhs.lo && lo ? std::max(*rhs.lo, *lo) : rhs.lo, rhs.hi});
  }
  SatisfyingInputs satisfyGreaterOrEqual(const Interval &rhs) const {
    return rhs.satisfyLessOrEqual(*this).swap();
  }
};

/// Interval state: map from Value* to Interval. Used as Sifa abstract state.
class IntervalState {
public:
  IntervalState() = default;
  explicit IntervalState(bool isBot) : isBottom_(isBot) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  std::optional<Interval> get(const llvm::Value *v) const {
    auto it = intervals_.find(v);
    if (it == intervals_.end()) return std::nullopt;
    return it->second;
  }
  void set(const llvm::Value *v, Interval i) { intervals_[v] = std::move(i); }
  const std::unordered_map<const llvm::Value *, Interval> &intervals() const {
    return intervals_;
  }

  bool operator==(const IntervalState &o) const {
    return isBottom_ == o.isBottom_ && intervals_ == o.intervals_;
  }

private:
  bool isBottom_ = false;
  std::unordered_map<const llvm::Value *, Interval> intervals_;
};

/// Interval domain implementing AbstractDomain<Transition, IntervalState>.
/// post(Edge) is identity; full instruction-level transfer can be added.
class IntervalDomain final : public AbstractDomain<Transition, IntervalState> {
public:
  using State = IntervalState;

  State top() const override { return State(false); }
  State bottom() const override {
    State s(true);
    return s;
  }
  bool isBottom(const State &s) const override { return s.isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (a.isBottom()) return true;
    if (b.isBottom()) return false;
    for (const auto &[v, ia] : a.intervals()) {
      auto ob = b.get(v);
      if (!ob) continue;
      const Interval &ib = *ob;
      if (ia.lo.has_value() && (!ib.lo.has_value() || *ia.lo < *ib.lo)) return false;
      if (ia.hi.has_value() && (!ib.hi.has_value() || *ia.hi > *ib.hi)) return false;
    }
    return true;
  }

  State join(const State &a, const State &b) const override {
    if (a.isBottom()) return b;
    if (b.isBottom()) return a;
    State r(false);
    for (const auto &[v, i] : a.intervals())
      r.set(v, i);
    for (const auto &[v, i] : b.intervals()) {
      auto oa = r.get(v);
      if (!oa) {
        r.set(v, i);
        continue;
      }
      Interval hull;
      hull.lo = (oa->lo && i.lo) ? std::optional<int64_t>(std::min(*oa->lo, *i.lo)) : std::nullopt;
      hull.hi = (oa->hi && i.hi) ? std::optional<int64_t>(std::max(*oa->hi, *i.hi)) : std::nullopt;
      r.set(v, hull);
    }
    return r;
  }

  State widen(const State &previous, const State &next) const override {
    if (next.isBottom()) return next;
    if (previous.isBottom()) return next;
    State r(false);
    for (const auto &[v, in] : next.intervals()) {
      auto op = previous.get(v);
      if (!op) {
        r.set(v, in);
        continue;
      }
      Interval w;
      if (op->lo.has_value() && in.lo.has_value() && *in.lo < *op->lo)
        w.lo = std::nullopt;
      else
        w.lo = in.lo;
      if (op->hi.has_value() && in.hi.has_value() && *in.hi > *op->hi)
        w.hi = std::nullopt;
      else
        w.hi = in.hi;
      r.set(v, w);
    }
    return r;
  }

  State post(const Transition &t, const State &in) const override {
    if (in.isBottom()) return in;
    if (t.kind == TransitionKind::Marker)
      return in;
    if (t.kind == TransitionKind::ReturnSummary)
      return in; // handled by interpreter via postReturn
    // Edge: identity (full block transfer can be added via t.source)
    return in;
  }

  State postCall(const State &callerState) const override { return callerState; }
  State postReturn(const State &callerState, const State &calleeSummary) const override {
    return join(callerState, calleeSummary);
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_INTERVALDOMAIN_H
