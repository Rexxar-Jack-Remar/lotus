/*
Lightweight Purity Analysis for LLVM IR functions. This analysis classifies functions into three categories:
- Pure: Functions that do not read or write any memory.
- ReadOnly: Functions that may read memory but do not write to it.
- Impure: Functions that may write to memory or have side effects.

TODO: maybe utilize alias analsyis results to improve precision, e.g., if a function only modifies memory that is not accessible from the caller, it can be considered pure.?

In lib/IR/MemorySSA, we even have a more fine-grained classification of memory accesses that utilzes the results of DSA pointer analysis. 
*/
#include "Analysis/Purity/FunctionPurityAnalysis.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

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

} // namespace

StringRef toString(PurityKind kind) {
  switch (kind) {
  case PurityKind::Pure:
    return "pure";
  case PurityKind::ReadOnly:
    return "readonly";
  case PurityKind::Impure:
    return "impure";
  }
  llvm_unreachable("unknown PurityKind");
}

FunctionPurityAnalysis::FunctionPurityAnalysis(Module &module) : module_(module) {}

void FunctionPurityAnalysis::run() {
  for (Function &function : module_) {
    if (function.isDeclaration()) {
      summaries_[&function] = classifyDeclaration(function);
    } else {
      summaries_[&function] = PurityKind::Impure;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (Function &function : module_) {
      if (function.isDeclaration()) {
        continue;
      }
      const PurityKind next = analyzeFunction(function);
      auto it = summaries_.find(&function);
      if (it == summaries_.end() || it->second != next) {
        summaries_[&function] = next;
        changed = true;
      }
    }
  }

  ran_ = true;
}

PurityKind FunctionPurityAnalysis::getPurity(const Function *function) const {
  if (!function || !ran_) {
    return PurityKind::Impure;
  }
  auto it = summaries_.find(function);
  return it == summaries_.end() ? PurityKind::Impure : it->second;
}

PurityKind FunctionPurityAnalysis::getCallPurity(const CallBase &call) const {
  return classifyCall(call);
}

bool FunctionPurityAnalysis::isPure(const Function *function) const {
  return getPurity(function) == PurityKind::Pure;
}

bool FunctionPurityAnalysis::isReadOnly(const Function *function) const {
  return getPurity(function) == PurityKind::ReadOnly;
}

bool FunctionPurityAnalysis::isAtMostReadOnly(const Function *function) const {
  return getPurity(function) != PurityKind::Impure;
}

PurityKind FunctionPurityAnalysis::analyzeFunction(const Function &function) const {
  PurityKind result = PurityKind::Pure;

  for (const Instruction &inst : instructions(function)) {
    if (isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) ||
        isa<AtomicCmpXchgInst>(inst) || isa<VAArgInst>(inst)) {
      return PurityKind::Impure;
    }

    if (const auto *call = dyn_cast<CallBase>(&inst)) {
      result = merge(result, classifyCall(*call));
      if (result == PurityKind::Impure) {
        return result;
      }
      continue;
    }

    if (inst.mayReadFromMemory()) {
      result = merge(result, PurityKind::ReadOnly);
    }
  }

  return result;
}

PurityKind FunctionPurityAnalysis::classifyDeclaration(
    const Function &function) const {
  if (function.onlyReadsMemory()) {
    return function.doesNotAccessMemory() ? PurityKind::Pure
                                          : PurityKind::ReadOnly;
  }

  if (function.isIntrinsic()) {
    return classifyIntrinsic(function);
  }

  static lotus::alias::AliasSpecManager specs;
  specs.initialize(module_);

  if (specs.isNoEffect(&function)) {
    return PurityKind::Pure;
  }

  const auto modRef = specs.getModRefInfo(&function);
  if (modRef.modifiedArgs.empty() && !modRef.modifiesReturn) {
    if (!modRef.referencedArgs.empty() || modRef.referencesReturn) {
      return PurityKind::ReadOnly;
    }
  }

  return PurityKind::Impure;
}

PurityKind FunctionPurityAnalysis::classifyCall(const CallBase &call) const {
  if (const auto *II = dyn_cast<IntrinsicInst>(&call)) {
    if (isBenignMemoryIntrinsic(*II)) {
      return PurityKind::Pure;
    }
  }

  const Function *callee = call.getCalledFunction();
  if (!callee) {
    return PurityKind::Impure;
  }

  if (callee == call.getFunction()) {
    return PurityKind::Pure;
  }

  if (callee->isIntrinsic()) {
    return classifyIntrinsic(*callee);
  }

  auto it = summaries_.find(callee);
  if (it != summaries_.end()) {
    return it->second;
  }

  return classifyDeclaration(*callee);
}

PurityKind FunctionPurityAnalysis::classifyIntrinsic(
    const Function &function) const {
  if (!function.isIntrinsic()) {
    return PurityKind::Impure;
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
    return PurityKind::Pure;
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
  case Intrinsic::vastart:
  case Intrinsic::vaend:
    return PurityKind::Impure;
  default:
    break;
  }

  if (function.onlyReadsMemory()) {
    return function.doesNotAccessMemory() ? PurityKind::Pure
                                          : PurityKind::ReadOnly;
  }

  return PurityKind::Impure;
}

PurityKind FunctionPurityAnalysis::merge(PurityKind lhs, PurityKind rhs) {
  return static_cast<int>(lhs) >= static_cast<int>(rhs) ? lhs : rhs;
}

} // namespace lotus::analysis::purity
