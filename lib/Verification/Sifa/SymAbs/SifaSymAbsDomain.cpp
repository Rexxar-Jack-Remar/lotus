#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/SymbolicAbstraction/Core/FunctionContext.h"

#include "llvm/IR/Function.h"

#include <stdexcept>
#include <string>

using namespace lotus::sifa;

SymAbsState SifaSymAbsDomain::top() const {
  llvm::Function *fn = fctx_.getFunction();
  if (!fn || fn->empty()) return nullptr;
  llvm::BasicBlock *entry = &*fn->begin();
  return makeTopAt(entry, /*after=*/false);
}

SymAbsState SifaSymAbsDomain::makeBottomAt(llvm::BasicBlock *bb, bool after) const {
  auto v = domainCtor_.makeBottom(fctx_, bb, after);
  return SymAbsState(v.release());
}

SymAbsState SifaSymAbsDomain::makeTopAt(llvm::BasicBlock *bb, bool after) const {
  auto v = domainCtor_.makeBottom(fctx_, bb, after);
  v->havoc();
  return SymAbsState(v.release());
}

SymAbsState SifaSymAbsDomain::post(const Label &t, const State &in) const {
  if (isBottom(in)) {
    return bottom();
  }
  if (t.kind == TransitionKind::Marker) {
    // Markers are handled in DagInterpreter; treat as identity here.
    return in;
  }
  if (t.kind != TransitionKind::Edge) {
    throw std::logic_error(
        "SifaSymAbsDomain only supports intraprocedural CFG edges; "
        "call-summary and enter-call transitions must be handled by Sifa.");
  }
  if (!t.source ||
      t.stopBefore != nullptr ||
      t.sourceOrdinal != 0 || (t.target && t.targetOrdinal != 0)) {
    throw std::logic_error(
        "SifaSymAbsDomain only supports whole-block edges between block-entry "
        "program points (or the synthetic EXIT).");
  }

  llvm::BasicBlock *src = t.source;
  llvm::BasicBlock *dst = t.target;

  std::set<symbolic_abstraction::Fragment::edge> edges;
  edges.insert({src, dst});
  const symbolic_abstraction::Fragment frag(fctx_, src, dst, edges, /*includes_end_body=*/false);

  // Result is a bottom at the end location (state at dst after phi nodes).
  auto out = domainCtor_.makeBottom(fctx_, dst, /*after=*/false);
  if (SifaLogger::isEnabled(SifaLogLevel::Debug)) {
    ++postCount_;
    if (postCount_ <= 10 || postCount_ % 25 == 0 || postCount_ == 11) {
      auto srcName = src ? (src->getName().empty() ? "(entry)" : src->getName().str()) : "?";
      auto dstName = dst ? (dst->getName().empty() ? "(exit)" : dst->getName().str()) : "EXIT";
      SifaLogger::debug("bestTransformer #" + std::to_string(postCount_) + ": " +
                        srcName + " -> " + dstName);
    }
  }
  analyzer_.bestTransformer(in.get(), frag, out.get());
  return SymAbsState(out.release());
}
