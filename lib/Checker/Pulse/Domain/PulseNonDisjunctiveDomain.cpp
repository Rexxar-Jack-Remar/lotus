#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

namespace pulse {

//===----------------------------------------------------------------------===//
// NonDisjunctiveDomain
//
// Tracks "must" information aggregated across disjuncts for efficiency. This
// is intentionally an *intersection*-like abstraction: it keeps only facts that
// are common to all observed states. This is useful for diagnostics such as
// unnecessary copies / const-ref suggestions.
//
// Note: This component is not where bug witnessability is decided; it is used
// for non-bug reports and summarization.
//===----------------------------------------------------------------------===//

void NonDisjunctiveDomain::addState(const AbductiveDomain &state) {
  if (!summary_) {
    // First state: use it as the summary
    summary_ = std::make_unique<AbductiveDomain>(state.clone());
    return;
  }

  AbductiveDomain intersection = summary_->clone();
  const AbductiveDomain &existing = *summary_;

  {
    Stack new_stack;
    for (const auto &kv : existing.getPostStack().getMap()) {
      const Address *rhs_addr = state.getPostStack().find(kv.first);
      if (rhs_addr && existing.getCanonical(kv.second.addr) ==
                          state.getCanonical(rhs_addr->addr)) {
        new_stack.add(kv.first, kv.second);
      }
    }
    intersection.getPostStack() = std::move(new_stack);
  }

  {
    Heap new_heap;
    for (const auto &kv : existing.getPostHeap().getEdges()) {
      AbstractValue lhs_from = existing.getCanonical(kv.first);
      for (const auto &edge_kv : kv.second) {
        const Address *rhs_target = state.getPostHeap().findEdge(lhs_from, edge_kv.first);
        if (!rhs_target) {
          continue;
        }
        if (!(existing.getCanonical(edge_kv.second.addr) ==
              state.getCanonical(rhs_target->addr))) {
          continue;
        }
        new_heap.addEdge(lhs_from, edge_kv.first, edge_kv.second);
      }
    }
    intersection.getPostHeap() = std::move(new_heap);
  }

  {
    AddressAttributes new_attrs;
    for (const auto &kv : existing.getPostAttrs().getAttrs()) {
      AbstractValue canon = existing.getCanonical(kv.first);
      AttributeSet common;
      for (Attribute attr : kv.second) {
        if (state.getPostAttrs().has(canon, attr)) {
          common.insert(attr);
        }
      }
      for (Attribute attr : common) {
        new_attrs.add(canon, attr);
      }
    }
    intersection.getPostAttrs() = std::move(new_attrs);
  }

  intersection.getTaintDomain().join(existing.getTaintDomain());
  intersection.canonicalize();
  summary_ = std::make_unique<AbductiveDomain>(std::move(intersection));
}

void NonDisjunctiveDomain::join(const NonDisjunctiveDomain &other) {
  if (other.isEmpty()) {
    return;
  }
  if (isEmpty()) {
    summary_ = std::make_unique<AbductiveDomain>(other.summary_->clone());
    copied_stores_ = other.copied_stores_;
    const_refable_params_ = other.const_refable_params_;
    return;
  }
  addState(*other.summary_);
  for (const auto *S : other.copied_stores_)
    copied_stores_.push_back(S);
  for (const auto *A : other.const_refable_params_)
    const_refable_params_.push_back(A);
}

NonDisjunctiveSummary
NonDisjunctiveSummary::join(const NonDisjunctiveSummary &s1,
                            const NonDisjunctiveSummary &s2) {
  if (s1.isEmpty()) {
    return s2.clone();
  }
  if (s2.isEmpty()) {
    return s1.clone();
  }

  // Compute intersection
  NonDisjunctiveDomain domain;
  domain.addState(*s1.getSummary());
  domain.addState(*s2.getSummary());

  if (domain.isEmpty()) {
    return NonDisjunctiveSummary();
  }

  return NonDisjunctiveSummary(
      std::make_unique<AbductiveDomain>(domain.getSummary()->clone()));
}

} // namespace pulse
