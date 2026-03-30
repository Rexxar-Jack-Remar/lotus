/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Invariants.h"

#include "Alias/Spec/AliasSpecManager.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

lotus::alias::AliasSpecManager &getLoopAnalysisSpecManager(void) {
  static lotus::alias::AliasSpecManager manager;
  return manager;
}

bool isAllocatorLike(CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = getLoopAnalysisSpecManager();
  return specManager.isAllocator(callee) || specManager.isDeallocator(callee) ||
         specManager.getCategory(callee) ==
             lotus::alias::FunctionCategory::Reallocator;
}

bool isPureLibraryCall(CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = getLoopAnalysisSpecManager();
  if (specManager.isNoEffect(callee)) {
    return true;
  }

  static const std::set<std::string> noellePureFallback{
      // ctype.h
      "isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph",
      "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit",
      "tolower", "toupper",
      // math.h
      "cos", "sin", "tan", "acos", "asin", "atan", "atan2", "cosh", "sinh",
      "tanh", "acosh", "asinh", "atanh", "exp", "expf", "ldexp", "log", "logf",
      "log10", "exp2", "expm1", "ilogb", "log1p", "log2", "logb", "scalbn",
      "scalbln", "pow", "sqrt", "cbrt", "hypot", "erf", "erfc", "tgamma",
      "lgamma", "ceil", "floor", "fmod", "trunc", "round", "lround", "llround",
      "nearbyint", "remainder", "copysign", "nextafter", "nexttoward", "fdim",
      "fmax", "fmin", "fabs", "abs", "fma", "fpclassify", "isfinite", "isinf",
      "isnan", "isnormal", "signbit", "isgreater", "isgreaterequal", "isless",
      "islessequal", "islessgreater", "isunordered",
      // time.h
      "clock", "difftime",
      // wctype.h
      "iswalnum", "iswalpha", "iswblank", "iswcntrl", "iswdigit", "iswgraph",
      "iswlower", "iswprint", "iswpunct", "iswspace", "iswupper", "iswxdigit",
      "towlower", "towupper", "iswctype", "towctrans",
      // stdlib.h / string.h
      "atoi", "atoll", "exit", "strcmp", "strncmp", "rand_r", "strlen"};
  return noellePureFallback.count(callee->getName().str()) != 0;
}

} // namespace

class InvariantManager::InvarianceChecker {
public:
  InvarianceChecker(LoopStructure *loop, LoopDependenceGraph *loopDG,
                    std::unordered_set<Instruction *> &invariants)
      : loop{loop}, loopDG{loopDG}, invariants{invariants} {
    for (auto *inst : loop->getInstructions()) {
      if (inst->isTerminator()) {
        continue;
      }

      if (auto *call = dyn_cast<CallInst>(inst)) {
        if (isAllocatorLike(call)) {
          this->notInvariants.insert(inst);
          continue;
        }
      }

      bool isPHI = false;
      if (auto *phi = dyn_cast<PHINode>(inst)) {
        isPHI = true;
        if (!arePHIIncomingValuesEquivalent(phi)) {
          continue;
        }
      }

      if (this->invariants.count(inst) != 0 ||
          this->notInvariants.count(inst) != 0) {
        continue;
      }

      this->dependencyValuesBeingChecked.clear();
      this->dependencyValuesBeingChecked.insert(inst);
      if (isPHI) {
        this->invariants.insert(inst);
      }

      auto canEvolve = this->loopDG->iterateOverDependencesTo(
          inst, false, true, true,
          [this](Value *toValue, LoopDependenceEdge *dep) {
            return this->isEvolvingValue(toValue, dep);
          });

      if (auto *call = dyn_cast<CallInst>(inst)) {
        auto *callee = call->getCalledFunction();
        if (callee != nullptr && callee->empty() && !isPureLibraryCall(call)) {
          canEvolve = true;
        }
      }

      if (canEvolve) {
        this->invariants.erase(inst);
        this->notInvariants.insert(inst);
      } else {
        this->invariants.insert(inst);
      }
    }
  }

private:
  bool isEvolvingValue(Value *toValue, LoopDependenceEdge *dep) {
    auto *toInst = dyn_cast<Instruction>(toValue);
    if (toInst == nullptr) {
      return false;
    }
    if (!this->loop->isIncluded(toInst)) {
      return false;
    }

    if (isa<StoreInst>(toInst)) {
      return true;
    }

    if (auto *call = dyn_cast<CallInst>(toInst)) {
      if (isAllocatorLike(call)) {
        return true;
      }
      if (isPureLibraryCall(call)) {
        return false;
      }
    }

    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return true;
    }

    bool isPHI = false;
    if (auto *phi = dyn_cast<PHINode>(toInst)) {
      isPHI = true;
      if (!arePHIIncomingValuesEquivalent(phi)) {
        return true;
      }
    }

    if (this->invariants.count(toInst) != 0) {
      return false;
    }
    if (this->notInvariants.count(toInst) != 0) {
      return true;
    }

    if (isPHI) {
      this->invariants.insert(toInst);
    }

    if (this->dependencyValuesBeingChecked.count(toInst) != 0) {
      return true;
    }
    this->dependencyValuesBeingChecked.insert(toInst);

    auto canEvolve = this->loopDG->iterateOverDependencesTo(
        toInst, false, true, true,
        [this](Value *nextValue, LoopDependenceEdge *nextDep) {
          return this->isEvolvingValue(nextValue, nextDep);
        });
    if (canEvolve) {
      this->invariants.erase(toInst);
      this->notInvariants.insert(toInst);
    } else {
      this->invariants.insert(toInst);
    }
    return canEvolve;
  }

  bool arePHIIncomingValuesEquivalent(PHINode *phi) const {
    std::unordered_set<Value *> incomingValues;
    for (auto &incomingUse : phi->incoming_values()) {
      incomingValues.insert(incomingUse.get());
    }
    if (incomingValues.empty()) {
      return false;
    }
    if (incomingValues.size() == 1) {
      return true;
    }

    Value *singleUniqueValue = *incomingValues.begin();
    for (auto *incomingValue : incomingValues) {
      if (incomingValue != singleUniqueValue) {
        singleUniqueValue = nullptr;
        break;
      }
    }
    if (singleUniqueValue != nullptr) {
      return true;
    }

    GlobalValue *singleGlobalLoaded = nullptr;
    for (auto *incomingValue : incomingValues) {
      auto *load = dyn_cast<LoadInst>(incomingValue);
      if (load == nullptr) {
        singleGlobalLoaded = nullptr;
        break;
      }
      auto *global = dyn_cast<GlobalValue>(load->getPointerOperand());
      if (global == nullptr) {
        singleGlobalLoaded = nullptr;
        break;
      }
      if (singleGlobalLoaded == nullptr || singleGlobalLoaded == global) {
        singleGlobalLoaded = global;
        continue;
      }
      singleGlobalLoaded = nullptr;
      break;
    }

    return singleGlobalLoaded != nullptr;
  }

  LoopStructure *loop;
  LoopDependenceGraph *loopDG;
  std::unordered_set<Instruction *> &invariants;
  std::unordered_set<Instruction *> notInvariants;
  std::unordered_set<Instruction *> dependencyValuesBeingChecked;
};

InvariantManager::InvariantManager(LoopStructure *loop,
                                   LoopDependenceGraph *loopDG)
    : loop{loop}, loopDG{loopDG} {
  for (auto *inst : loop->getInstructions()) {
    if (loop->isLoopInvariant(inst)) {
      this->invariants.insert(inst);
    }
  }

  InvarianceChecker checker{loop, loopDG, this->invariants};
  (void)checker;
}

bool InvariantManager::isLoopInvariant(Value *value) const {
  if (!isa<Instruction>(value)) {
    return true;
  }
  auto *inst = cast<Instruction>(value);
  if (!this->loop->isIncluded(inst)) {
    return true;
  }
  return this->invariants.count(inst) != 0;
}

bool InvariantManager::isLoopInvariant(LoopSCC *scc) const {
  auto interrupted = scc->iterateOverInstructions(
      [this](Instruction *inst) { return !this->isLoopInvariant(inst); });
  return !interrupted;
}

std::unordered_set<Instruction *>
InvariantManager::getLoopInstructionsThatAreLoopInvariants(void) const {
  return this->invariants;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
