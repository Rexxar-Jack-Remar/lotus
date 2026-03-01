//===-- Verification/Sifa/SymAbs/SifaSymAbsDomain.h -----------------------===//
//
// Intraprocedural Sifa helper domain implemented using SymbolicAbstraction's
// AbstractValue on whole-block CFG edges.
//
// This is intentionally not a full Sifa interprocedural domain adapter:
// call-summary and enter-call transitions are rejected by post().
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Log/SifaLogger.h"

#include "Verification/SymbolicAbstraction/Analyzers/Analyzer.h"
#include "Verification/SymbolicAbstraction/Core/DomainConstructor.h"
#include "Verification/SymbolicAbstraction/Core/Fragment.h"

#include <cstdint>
#include <memory>
#include <set>

namespace symbolic_abstraction {
class FunctionContext;
} // namespace symbolic_abstraction

namespace lotus {
namespace sifa {

using SymAbsState = std::shared_ptr<symbolic_abstraction::AbstractValue>;

class SifaSymAbsDomain final : public AbstractDomain<Transition, SymAbsState> {
public:
  using Label = Transition;
  using State = SymAbsState;

  /// post() logs progress via SifaLogger when log level is Debug or higher.
  SifaSymAbsDomain(const symbolic_abstraction::FunctionContext &fctx,
                   const symbolic_abstraction::DomainConstructor &domainCtor,
                   const symbolic_abstraction::Analyzer &analyzer)
      : fctx_(fctx), domainCtor_(domainCtor), analyzer_(analyzer) {}

  State top() const override;
  State bottom() const override { return nullptr; }
  bool isBottom(const State &s) const override { return !s || s->isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return isBottom(a);
    return (*a) <= (*b);
  }

  State join(const State &a, const State &b) const override {
    if (isBottom(a)) return b;
    if (isBottom(b)) return a;
    std::unique_ptr<symbolic_abstraction::AbstractValue> out(a->clone());
    out->joinWith(*b);
    return State(out.release());
  }

  State widen(const State &previous, const State &next) const override {
    if (isBottom(previous)) return next;
    if (isBottom(next)) return previous;
    std::unique_ptr<symbolic_abstraction::AbstractValue> out(previous->clone());
    out->joinWith(*next);
    out->widen();
    return State(out.release());
  }

  State post(const Label &t, const State &in) const override;

  /// Create a location-appropriate bottom value (SymbolicAbstraction makeBottom).
  State makeBottomAt(llvm::BasicBlock *bb, bool after) const;

  /// Create a location-appropriate top value.
  State makeTopAt(llvm::BasicBlock *bb, bool after) const;

private:
  const symbolic_abstraction::FunctionContext &fctx_;
  const symbolic_abstraction::DomainConstructor &domainCtor_;
  const symbolic_abstraction::Analyzer &analyzer_;
  mutable std::uint64_t postCount_ = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H
