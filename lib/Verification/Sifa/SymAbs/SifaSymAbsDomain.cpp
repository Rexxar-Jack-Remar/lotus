#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

#include "Verification/SymbolicAbstraction/Core/FunctionContext.h"

#include "llvm/IR/Function.h"

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
  if (t.kind != TransitionKind::Edge) {
    // Markers are handled in DagInterpreter; treat as identity here.
    return in;
  }

  llvm::BasicBlock *src = t.source;
  llvm::BasicBlock *dst = t.target;

  std::set<symbolic_abstraction::Fragment::edge> edges;
  edges.insert({src, dst});
  const symbolic_abstraction::Fragment frag(fctx_, src, dst, edges, /*includes_end_body=*/false);

  // Result is a bottom at the end location (state at dst after phi nodes).
  auto out = domainCtor_.makeBottom(fctx_, dst, /*after=*/false);
  analyzer_.bestTransformer(in.get(), frag, out.get());
  return SymAbsState(out.release());
}
