/*
Purity analysis for LLVM IR functions.

This classifier targets source-level attribute candidates:
- Const: no reachable-memory reads or writes, and no observable side effects.
- Pure: may read reachable memory, but does not write reachable memory and has
  no observable side effects.
- Impure: writes reachable memory or has observable side effects.
- Unknown: current summaries are insufficient to prove const/pure safely.
*/
#include "Analysis/Purity/FunctionPurityAnalysis.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"
#include "Analysis/Purity/MemorySSAPuritySummary.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

bool isBenignMemoryIntrinsic(const IntrinsicInst &II) {
  switch (II.getIntrinsicID()) {
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::assume:
  case Intrinsic::expect:
    return true;
  default:
    return false;
  }
}

bool isShadowMemCall(const CallBase &call) {
  const Function *callee = call.getCalledFunction();
  return callee && callee->getName().startswith("shadow.mem");
}

FunctionEffectSummary constSummary(bool fromMemorySSA = false) {
  FunctionEffectSummary summary;
  summary.fromMemorySSA = fromMemorySSA;
  return summary;
}

FunctionEffectSummary pureSummary(bool fromMemorySSA = false) {
  FunctionEffectSummary summary;
  summary.readsReachableMemory = true;
  summary.fromMemorySSA = fromMemorySSA;
  return summary;
}

FunctionEffectSummary impureSummary(bool fromMemorySSA = false,
                                    bool readsMemory = false) {
  FunctionEffectSummary summary;
  summary.readsReachableMemory = readsMemory;
  summary.writesReachableMemory = true;
  summary.fromMemorySSA = fromMemorySSA;
  return summary;
}

FunctionEffectSummary unknownSummary(bool fromMemorySSA = false,
                                     bool readsMemory = false) {
  FunctionEffectSummary summary;
  summary.readsReachableMemory = readsMemory;
  summary.hasUnknownEffects = true;
  summary.fromMemorySSA = fromMemorySSA;
  return summary;
}

bool hasObservableCallEffects(const CallBase &call) {
  if (call.isInlineAsm()) {
    return true;
  }
  if (call.hasFnAttr(Attribute::ReturnsTwice)) {
    return true;
  }
  if (call.isConvergent()) {
    return true;
  }
  return false;
}

bool hasExplicitMemoryAttribute(const Function &function) {
  return function.hasFnAttribute(Attribute::ReadNone) ||
         function.hasFnAttribute(Attribute::ReadOnly) ||
         function.doesNotAccessMemory() || function.onlyReadsMemory();
}

} // namespace

StringRef toString(PurityKind kind) {
  switch (kind) {
  case PurityKind::Const:
    return "const";
  case PurityKind::Pure:
    return "pure";
  case PurityKind::Impure:
    return "impure";
  case PurityKind::Unknown:
    return "unknown";
  }
  llvm_unreachable("unknown PurityKind");
}

PurityKind FunctionEffectSummary::getPurityKind() const {
  if (writesReachableMemory || hasObservableSideEffects) {
    return PurityKind::Impure;
  }
  if (hasUnknownEffects) {
    return PurityKind::Unknown;
  }
  if (readsReachableMemory) {
    return PurityKind::Pure;
  }
  return PurityKind::Const;
}

bool FunctionEffectSummary::isConstCandidate() const {
  return getPurityKind() == PurityKind::Const;
}

bool FunctionEffectSummary::isPureCandidate() const {
  const PurityKind kind = getPurityKind();
  return kind == PurityKind::Const || kind == PurityKind::Pure;
}

FunctionPurityAnalysis::FunctionPurityAnalysis(Module &module,
                                               MemorySSAMode memorySSAMode)
    : module_(module), memorySSAMode_(memorySSAMode) {
  if (memorySSAMode_ == MemorySSAMode::UseIfAvailable) {
    memorySSAProvider_ =
        std::make_unique<MemorySSAPuritySummaryProvider>(module_);
  }
}

FunctionPurityAnalysis::~FunctionPurityAnalysis() = default;

void FunctionPurityAnalysis::run() {
  summaries_.clear();

  for (Function &function : module_) {
    summaries_[&function] =
        function.isDeclaration() ? classifyDeclaration(function)
                                 : initialSummary(function);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (Function &function : module_) {
      if (function.isDeclaration()) {
        continue;
      }
      const FunctionEffectSummary next = analyzeFunction(function);
      auto it = summaries_.find(&function);
      if (it == summaries_.end() ||
          it->second.readsReachableMemory != next.readsReachableMemory ||
          it->second.writesReachableMemory != next.writesReachableMemory ||
          it->second.hasObservableSideEffects != next.hasObservableSideEffects ||
          it->second.hasUnknownEffects != next.hasUnknownEffects ||
          it->second.fromMemorySSA != next.fromMemorySSA) {
        summaries_[&function] = next;
        changed = true;
      }
    }
  }

  ran_ = true;
}

PurityKind FunctionPurityAnalysis::getPurity(const Function *function) const {
  return getEffects(function).getPurityKind();
}

PurityKind FunctionPurityAnalysis::getCallPurity(const CallBase &call) const {
  return classifyCall(call).getPurityKind();
}

FunctionEffectSummary
FunctionPurityAnalysis::getEffects(const Function *function) const {
  if (!function || !ran_) {
    return unknownSummary();
  }
  auto it = summaries_.find(function);
  return it == summaries_.end() ? unknownSummary() : it->second;
}

bool FunctionPurityAnalysis::hasMemorySSASummaries() const {
  return memorySSAProvider_ && memorySSAProvider_->hasInstrumentedIR();
}

bool FunctionPurityAnalysis::isConst(const Function *function) const {
  return getPurity(function) == PurityKind::Const;
}

bool FunctionPurityAnalysis::isPure(const Function *function) const {
  return getPurity(function) == PurityKind::Pure;
}

bool FunctionPurityAnalysis::isAtMostPure(const Function *function) const {
  const PurityKind kind = getPurity(function);
  return kind == PurityKind::Const || kind == PurityKind::Pure;
}

bool FunctionPurityAnalysis::isKnown(const Function *function) const {
  return getPurity(function) != PurityKind::Unknown;
}

FunctionEffectSummary
FunctionPurityAnalysis::initialSummary(const Function &function) const {
  if (hasMemorySSASummaries()) {
    if (auto summary = memorySSAProvider_->getFunctionSummary(function)) {
      if (summary->writesReachableMemory) {
        return impureSummary(true, summary->readsReachableMemory);
      }
      return summary->readsReachableMemory ? pureSummary(true)
                                           : constSummary(true);
    }
  }
  return constSummary();
}

FunctionEffectSummary
FunctionPurityAnalysis::analyzeFunction(const Function &function) const {
  FunctionEffectSummary result = initialSummary(function);

  for (const Instruction &inst : instructions(function)) {
    if (isa<ResumeInst>(inst) || isa<CleanupReturnInst>(inst) ||
        isa<CatchReturnInst>(inst) || isa<FenceInst>(inst) ||
        isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst) ||
        isa<VAArgInst>(inst)) {
      result.hasObservableSideEffects = true;
      return result;
    }

    if (const auto *load = dyn_cast<LoadInst>(&inst)) {
      if (load->isVolatile() || load->isAtomic()) {
        result.hasObservableSideEffects = true;
        return result;
      }
      if (!hasMemorySSASummaries()) {
        result = merge(result, pureSummary());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      if (store->isVolatile() || store->isAtomic()) {
        result.hasObservableSideEffects = true;
        return result;
      }
      if (!hasMemorySSASummaries()) {
        return impureSummary();
      }
      continue;
    }

    if (const auto *call = dyn_cast<CallBase>(&inst)) {
      if (isShadowMemCall(*call)) {
        continue;
      }
      result = merge(result, classifyCall(*call));
      if (result.getPurityKind() == PurityKind::Impure) {
        return result;
      }
      continue;
    }

    if (inst.mayReadFromMemory() && !hasMemorySSASummaries()) {
      result = merge(result, pureSummary());
    }
  }

  return result;
}

FunctionEffectSummary FunctionPurityAnalysis::classifyDeclaration(
    const Function &function, const CallBase *callSite) const {
  if (function.isIntrinsic()) {
    return classifyIntrinsic(function);
  }

  if ((function.doesNotAccessMemory() ||
       function.hasFnAttribute(Attribute::ReadNone))) {
    return constSummary();
  }

  if ((function.onlyReadsMemory() ||
       function.hasFnAttribute(Attribute::ReadOnly))) {
    return pureSummary();
  }

  static lotus::alias::AliasSpecManager specs;
  specs.initialize(module_);

  if (specs.isNoEffect(&function)) {
    return constSummary();
  }

  const auto modRef = specs.getModRefInfo(&function);
  if (modRef.modifiedArgs.empty() && !modRef.modifiesReturn &&
      (!modRef.referencedArgs.empty() || modRef.referencesReturn)) {
    return pureSummary();
  }
  if (!modRef.modifiedArgs.empty() || modRef.modifiesReturn) {
    return impureSummary(false, !modRef.referencedArgs.empty() ||
                                    modRef.referencesReturn);
  }

  if (callSite && hasMemorySSASummaries()) {
    if (auto callSummary = memorySSAProvider_->getCallSummary(*callSite)) {
      if (callSummary->writesReachableMemory) {
        return impureSummary(true, callSummary->readsReachableMemory);
      }
      if (callSummary->readsReachableMemory) {
        if (hasExplicitMemoryAttribute(function)) {
          return pureSummary(true);
        }
        return unknownSummary(true, true);
      }
      if (!hasExplicitMemoryAttribute(function)) {
        return unknownSummary(true);
      }
    }
  }

  return unknownSummary();
}

FunctionEffectSummary
FunctionPurityAnalysis::classifyCall(const CallBase &call) const {
  if (hasObservableCallEffects(call)) {
    FunctionEffectSummary summary = impureSummary();
    summary.hasObservableSideEffects = true;
    return summary;
  }

  if (const auto *II = dyn_cast<IntrinsicInst>(&call)) {
    if (isBenignMemoryIntrinsic(*II)) {
      return constSummary();
    }
  }

  const Function *callee = call.getCalledFunction();
  if (!callee) {
    if (call.doesNotAccessMemory()) {
      return constSummary();
    }
    if (call.onlyReadsMemory()) {
      return pureSummary();
    }
    return unknownSummary();
  }

  if (callee == call.getFunction()) {
    return constSummary();
  }

  if (callee->isIntrinsic()) {
    return classifyIntrinsic(*callee);
  }

  if (callee->isDeclaration()) {
    return classifyDeclaration(*callee, &call);
  }

  auto it = summaries_.find(callee);
  return it == summaries_.end() ? unknownSummary() : it->second;
}

FunctionEffectSummary
FunctionPurityAnalysis::classifyIntrinsic(const Function &function) const {
  if (!function.isIntrinsic()) {
    return unknownSummary();
  }

  switch (function.getIntrinsicID()) {
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::assume:
  case Intrinsic::expect:
    return constSummary();
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
  case Intrinsic::vastart:
  case Intrinsic::vaend:
    return impureSummary();
  default:
    break;
  }

  if ((function.doesNotAccessMemory() ||
       function.hasFnAttribute(Attribute::ReadNone))) {
    return constSummary();
  }
  if ((function.onlyReadsMemory() ||
       function.hasFnAttribute(Attribute::ReadOnly))) {
    return pureSummary();
  }

  return unknownSummary();
}

FunctionEffectSummary FunctionPurityAnalysis::merge(
    const FunctionEffectSummary &lhs, const FunctionEffectSummary &rhs) {
  FunctionEffectSummary merged;
  merged.readsReachableMemory =
      lhs.readsReachableMemory || rhs.readsReachableMemory;
  merged.writesReachableMemory =
      lhs.writesReachableMemory || rhs.writesReachableMemory;
  merged.hasObservableSideEffects =
      lhs.hasObservableSideEffects || rhs.hasObservableSideEffects;
  merged.hasUnknownEffects = lhs.hasUnknownEffects || rhs.hasUnknownEffects;
  merged.fromMemorySSA = lhs.fromMemorySSA || rhs.fromMemorySSA;
  return merged;
}

} // namespace lotus::analysis::purity
