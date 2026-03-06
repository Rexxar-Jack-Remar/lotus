#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/InstructionSemantics.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace lotus::sifa;

namespace {

const llvm::Instruction *firstNonPhi(const llvm::BasicBlock *bb) {
  if (!bb)
    return nullptr;
  for (const llvm::Instruction &I : *bb) {
    if (!llvm::isa<llvm::PHINode>(&I))
      return &I;
  }
  return nullptr;
}

llvm::BasicBlock *transitionTarget(const Transition &t) {
  return t.target ? t.target : symabs_ai::Fragment::EXIT;
}

bool isBlockEntryPoint(const Transition &t) {
  return t.sourceOrdinal == 0 && (!t.target || t.targetOrdinal == 0);
}

z3::expr directCallSummaryFormula(const symabs_ai::FunctionContext &fctx,
                                  const symabs_ai::ValueMapping &vmBefore,
                                  const symabs_ai::ValueMapping &vmAfter,
                                  const llvm::CallBase &call) {
  auto &z3 = fctx.getZ3();
  llvm::Function *callee = call.getCalledFunction();
  if (!callee) {
    return z3.bool_val(true);
  }

  const auto &mctx = fctx.getModuleContext();
  z3::expr rawExpr = mctx.formulaFor(callee);
  z3::expr_vector src(z3), dst(z3);

  auto *formalIt = callee->arg_begin();
  auto *formalEnd = callee->arg_end();
  const auto *actualIt = call.arg_begin();
  const auto *actualEnd = call.arg_end();
  for (; formalIt != formalEnd && actualIt != actualEnd;
       ++formalIt, ++actualIt) {
    llvm::Value *actual = *actualIt;
    if (actual->getType()->isMetadataTy()) {
      continue;
    }
    z3::expr actualExpr = vmBefore.getFullRepresentation(actual);
    src.push_back(
        z3.constant(formalIt->getName().str().c_str(), actualExpr.get_sort()));
    dst.push_back(actualExpr);
  }

  if (!call.getType()->isVoidTy()) {
    z3::expr resultExpr =
        vmAfter.getFullRepresentation(const_cast<llvm::CallBase *>(&call));
    src.push_back(z3.constant(mctx.getReturnSymbol(), resultExpr.get_sort()));
    dst.push_back(resultExpr);
  }

  z3::expr result = rawExpr.substitute(src, dst);
  if (callee->onlyReadsMemory() || callee->doesNotAccessMemory() ||
      call.getMetadata("symbolic_abstraction")) {
    result = result && (vmBefore.memory() == vmAfter.memory());
  }
  return result;
}

z3::expr
preserveUnchangedRepresentedValues(const symabs_ai::FunctionContext &fctx,
                                   const symabs_ai::Fragment &frag,
                                   const symabs_ai::ValueMapping &vmBefore,
                                   const symabs_ai::ValueMapping &vmAfter) {
  z3::expr preserved = fctx.getZ3().bool_val(true);
  for (const auto &value : fctx.representedValues()) {
    llvm::Value *llvmValue = value;
    if (!frag.defines(llvmValue)) {
      preserved = preserved && (vmBefore.getFullRepresentation(llvmValue) ==
                                vmAfter.getFullRepresentation(llvmValue));
    }
  }
  return preserved;
}

} // namespace

SymAbsState SifaSymAbsDomain::top() const {
  llvm::Function *fn = fctx_.getFunction();
  if (!fn || fn->empty())
    return nullptr;
  llvm::BasicBlock *entry = &*fn->begin();
  return makeTopAt(entry, /*after=*/false);
}

SymAbsState SifaSymAbsDomain::makeBottomAt(llvm::BasicBlock *bb,
                                           bool after) const {
  auto v = domainCtor_.makeBottom(fctx_, bb, after);
  return SymAbsState(v.release());
}

SymAbsState SifaSymAbsDomain::makeTopAt(llvm::BasicBlock *bb,
                                        bool after) const {
  auto v = domainCtor_.makeBottom(fctx_, bb, after);
  v->havoc();
  return SymAbsState(v.release());
}

bool SifaSymAbsDomain::supportsBestTransformer(const Label &t) const {
  if (t.kind != TransitionKind::Edge || !t.source)
    return false;
  if (t.stopBefore != nullptr)
    return false;
  if (!isBlockEntryPoint(t))
    return false;
  const llvm::Instruction *blockStart = firstNonPhi(t.source);
  return t.segmentStart == nullptr || t.segmentStart == blockStart;
}

SymAbsState SifaSymAbsDomain::fallbackReturnSummary(const Label &t,
                                                    const State &in) const {
  const auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(t.call);
  if (!call) {
    return makeTopAt(transitionTarget(t), /*after=*/false);
  }
  if (const auto *callInst = llvm::dyn_cast<llvm::CallInst>(call)) {
    auto frag =
        symabs_ai::FragmentDecomposition::FragmentForBody(fctx_, t.source);
    symabs_ai::InstructionSemantics instSem(fctx_, frag);
    auto vmIn = symabs_ai::ValueMapping::before(
        fctx_, frag, const_cast<llvm::CallInst *>(callInst));
    auto vmOut = symabs_ai::ValueMapping::after(
        fctx_, frag, const_cast<llvm::CallInst *>(callInst));

    z3::expr phi = in->toFormula(vmIn, fctx_.getZ3()) &&
                   instSem.visit(*const_cast<llvm::CallInst *>(callInst));

    auto out = makeBottomAt(t.source, /*after=*/true);
    analyzer_.strongestConsequence(out.get(), phi, vmOut);
    return out;
  }

  if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
    std::set<symabs_ai::Fragment::edge> edges;
    edges.insert({t.source, transitionTarget(t)});
    symabs_ai::Fragment frag(fctx_, t.source, transitionTarget(t), edges,
                             /*includes_end_body=*/false);
    symabs_ai::InstructionSemantics instSem(fctx_, frag);

    auto vmIn = symabs_ai::ValueMapping::before(
        fctx_, frag, const_cast<llvm::InvokeInst *>(invoke));
    auto vmOut = symabs_ai::ValueMapping::atEnd(fctx_, frag);

    z3::expr phi = in->toFormula(vmIn, fctx_.getZ3());
    phi = phi && directCallSummaryFormula(fctx_, vmIn, vmOut, *invoke);
    phi = phi && preserveUnchangedRepresentedValues(fctx_, frag, vmIn, vmOut);

    for (llvm::BasicBlock *succ : llvm::successors(t.source)) {
      z3::expr edgeVar = fctx_.getEdgeVariable(t.source, succ);
      phi = phi && (succ == t.target ? edgeVar : !edgeVar);
    }
    for (const auto &edge : frag.edges()) {
      for (llvm::Instruction &I : frag.edgePhis(edge)) {
        phi = phi && instSem.visit(I);
      }
    }

    auto out = makeBottomAt(t.target, /*after=*/false);
    analyzer_.strongestConsequence(out.get(), phi, vmOut);
    return out;
  }

  return makeTopAt(transitionTarget(t), /*after=*/false);
}

SymAbsState SifaSymAbsDomain::fallbackPost(const Label &t,
                                           const State &in) const {
  if (!t.source) {
    throw std::logic_error("SifaSymAbs fallback requires a source block");
  }

  const bool crossesEdge = transitionTarget(t) != t.source;
  std::vector<symabs_ai::Fragment::edge> edges;
  if (crossesEdge) {
    edges.push_back({t.source, transitionTarget(t)});
  }

  symabs_ai::Fragment frag(fctx_, t.source, transitionTarget(t), edges,
                           /*includes_end_body=*/false);
  symabs_ai::InstructionSemantics instSem(fctx_, frag);

  const llvm::Instruction *segmentStart =
      t.segmentStart ? t.segmentStart : firstNonPhi(t.source);
  if (!segmentStart) {
    return makeTopAt(transitionTarget(t), /*after=*/false);
  }
  if (t.stopBefore == segmentStart) {
    return in;
  }

  auto vmIn = symabs_ai::ValueMapping::before(
      fctx_, frag, const_cast<llvm::Instruction *>(segmentStart));
  z3::expr phi = in->toFormula(vmIn, fctx_.getZ3());

  for (auto it = const_cast<llvm::Instruction *>(segmentStart)->getIterator(),
            end = t.source->end();
       it != end; ++it) {
    llvm::Instruction &I = *it;
    if (&I == t.stopBefore) {
      break;
    }
    phi = phi && instSem.visit(I);
  }

  if (crossesEdge) {
    const llvm::Instruction *terminator = t.source->getTerminator();
    if (terminator && terminator != t.stopBefore) {
      z3::expr chosen = fctx_.getEdgeVariable(t.source, transitionTarget(t));
      for (llvm::BasicBlock *succ : llvm::successors(t.source)) {
        z3::expr edgeVar = fctx_.getEdgeVariable(t.source, succ);
        phi = phi && (succ == t.target ? edgeVar : !edgeVar);
      }
      if (llvm::isa<llvm::ReturnInst>(terminator) && !t.target) {
        phi = phi && chosen;
      }

      auto vmBeforeTerm = symabs_ai::ValueMapping::before(
          fctx_, frag, const_cast<llvm::Instruction *>(terminator));
      auto vmOut = symabs_ai::ValueMapping::atEnd(fctx_, frag);
      phi = phi &&
            fctx_.getMemoryModel().copy(vmBeforeTerm.memory(), vmOut.memory());

      for (const auto &edge : frag.edges()) {
        for (llvm::Instruction &I : frag.edgePhis(edge)) {
          phi = phi && instSem.visit(I);
        }
      }

      auto out = makeBottomAt(t.target, /*after=*/false);
      analyzer_.strongestConsequence(out.get(), phi, vmOut);
      return out;
    }
  }

  auto out = makeBottomAt(t.source, /*after=*/true);
  auto vmOut = t.stopBefore ? symabs_ai::ValueMapping::before(
                                  fctx_, frag,
                                  const_cast<llvm::Instruction *>(t.stopBefore))
                            : symabs_ai::ValueMapping::atEnd(fctx_, frag);
  analyzer_.strongestConsequence(out.get(), phi, vmOut);
  return out;
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
    return fallbackPost(t, in);
  }
  if (!supportsBestTransformer(t)) {
    return fallbackPost(t, in);
  }

  llvm::BasicBlock *src = t.source;
  llvm::BasicBlock *dst = t.target;

  std::set<symabs_ai::Fragment::edge> edges;
  edges.insert({src, dst});
  const symabs_ai::Fragment frag(fctx_, src, dst, edges,
                                 /*includes_end_body=*/false);

  // Result is a bottom at the end location (state at dst after phi nodes).
  auto out = domainCtor_.makeBottom(fctx_, dst, /*after=*/false);
  if (SifaLogger::isEnabled(SifaLogLevel::Debug)) {
    ++postCount_;
    if (postCount_ <= 10 || postCount_ % 25 == 0 || postCount_ == 11) {
      auto srcName =
          src ? (src->getName().empty() ? "(entry)" : src->getName().str())
              : "?";
      auto dstName =
          dst ? (dst->getName().empty() ? "(exit)" : dst->getName().str())
              : "EXIT";
      SifaLogger::debug("bestTransformer #" + std::to_string(postCount_) +
                        ": " + srcName + " -> " + dstName);
    }
  }
  analyzer_.bestTransformer(in.get(), frag, out.get());
  return SymAbsState(out.release());
}

SymAbsState SifaSymAbsDomain::postCall(const Label &t,
                                       const State &callerState) const {
  if (isBottom(callerState))
    return bottom();
  if (t.kind == TransitionKind::ReturnSummary && t.call) {
    return fallbackReturnSummary(t, callerState);
  }
  if (t.kind == TransitionKind::EnterCall && t.target) {
    return makeTopAt(t.target, /*after=*/false);
  }
  return callerState;
}
