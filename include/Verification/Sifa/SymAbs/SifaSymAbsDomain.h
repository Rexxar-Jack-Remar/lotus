//===-- Verification/Sifa/SymAbs/SifaSymAbsDomain.h -----------------------===//
//
// Intraprocedural Sifa helper domain implemented using SymAbsAI's
// AbstractValue on whole-block CFG edges, with an SMT-based fallback for
// segmented intra-block transfers that do not fit bestTransformer's fragment
// shape.
//
// This is intentionally not a full Sifa interprocedural domain adapter.
// Enter-call transitions still fall back to a coarse top-at-entry state.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include <cstdint>
#include <memory>
#include <set>

namespace symabs_ai {
class FunctionContext;
} // namespace symabs_ai

namespace lotus {
namespace sifa {

using SymAbsState = std::shared_ptr<symabs_ai::AbstractValue>;

class SifaSymAbsDomain final : public AbstractDomain<Transition, SymAbsState> {
public:
  using Label = Transition;
  using State = SymAbsState;

  /// post() logs progress via SifaLogger when log level is Debug or higher.
  SifaSymAbsDomain(const symabs_ai::FunctionContext &fctx,
                   const symabs_ai::DomainConstructor &domainCtor,
                   const symabs_ai::Analyzer &analyzer)
      : fctx_(fctx), domainCtor_(domainCtor), analyzer_(analyzer) {}

  State top() const override;
  State bottom() const override { return nullptr; }
  bool isBottom(const State &s) const override { return !s || s->isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (isBottom(a))
      return true;
    if (isBottom(b))
      return isBottom(a);
    return (*a) <= (*b);
  }

  State join(const State &a, const State &b) const override {
    if (isBottom(a))
      return b;
    if (isBottom(b))
      return a;
    std::unique_ptr<symabs_ai::AbstractValue> out(a->clone());
    out->joinWith(*b);
    return State(out.release());
  }

  State widen(const State &previous, const State &next) const override {
    if (isBottom(previous))
      return next;
    if (isBottom(next))
      return previous;
    std::unique_ptr<symabs_ai::AbstractValue> out(previous->clone());
    out->joinWith(*next);
    out->widen();
    return State(out.release());
  }

  State post(const Label &t, const State &in) const override;
  State postCall(const Label &t, const State &callerState) const override;

  /// Create a location-appropriate bottom value (SymAbsAI makeBottom).
  State makeBottomAt(llvm::BasicBlock *bb, bool after) const;

  /// Create a location-appropriate top value.
  State makeTopAt(llvm::BasicBlock *bb, bool after) const;

private:
  State fallbackPost(const Label &t, const State &in) const;
  State fallbackReturnSummary(const Label &t, const State &in) const;
  bool supportsBestTransformer(const Label &t) const;

  const symabs_ai::FunctionContext &fctx_;
  const symabs_ai::DomainConstructor &domainCtor_;
  const symabs_ai::Analyzer &analyzer_;
  mutable std::uint64_t postCount_ = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H
