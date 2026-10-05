#pragma once

#include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
#include "Analysis/Memory/HeapPointerAnalysis.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Utils/LLVM/CallUtils.h"

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Demangle/Demangle.h>

namespace lotus {
namespace gsaf {

inline Function *calledFunction(Value *value) {
  auto *call = dyn_cast_or_null<CallBase>(value);
  return call ? call->getCalledFunction() : nullptr;
}
inline StringRef calledName(Value *value) {
  auto *function = calledFunction(value);
  return function ? function->getName() : StringRef{};
}
inline bool isDivision(Value *value) {
  auto *instruction = dyn_cast_or_null<Instruction>(value);
  if (!instruction)
    return false;
  switch (instruction->getOpcode()) {
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
    return true;
  default:
    return false;
  }
}
inline bool isFreeCall(Instruction *instruction,
                       TargetLibraryInfoWrapperPass *wrapper) {
  return llvm::isFreeCall(instruction,
                          &wrapper->getTLI(*instruction->getFunction()));
}
inline std::string demangle_function(StringRef name, bool = true, bool = false,
                                     bool = true) {
  return llvm::demangle(name.str());
}
inline std::string demangle_function(const Function *function, bool = true,
                                     bool = false, bool = true) {
  return function ? llvm::demangle(function->getName().str()) : std::string{};
}
inline bool isCallArgument(Value *value, Value *operation, size_t index) {
  auto *call = dyn_cast_or_null<CallBase>(operation);
  return call && index < call->arg_size() &&
         value == call->getArgOperand(index);
}

inline std::unique_ptr<HeapPointerAnalysis> heapAnalysis(Pass *pass) {
  auto &models = pass->getAnalysis<GSAFModels>();
  auto *module = static_cast<GSAFChecker *>(pass)->getModule();
  std::vector<std::string> allocators, releases;
  models.getAllMemoryAllocs(allocators);
  models.getAllMemoryFrees(releases);
  auto analysis = std::make_unique<HeapPointerAnalysis>();
  analysis->analyze(*module, pass->getAnalysis<DyckAliasAnalysis>(), allocators,
                    releases);
  return analysis;
}

inline std::string demangleUnqualifiedName(StringRef name) {
  llvm::ItaniumPartialDemangler demangler;
  if (demangler.partialDemangle(name.str().c_str()))
    return name.str();
  char *base = demangler.getFunctionBaseName(nullptr, nullptr);
  std::string result = base ? base : name.str();
  std::free(base);
  return result;
}
inline bool is_mangled_ctor(StringRef name) {
  llvm::ItaniumPartialDemangler demangler;
  return !demangler.partialDemangle(name.str().c_str()) &&
         demangler.isCtorOrDtor() &&
         demangleUnqualifiedName(name).find('~') == std::string::npos;
}

inline bool is_mangled_dtor(const Function *function) {
  if (!function)
    return false;
  llvm::ItaniumPartialDemangler demangler;
  return !demangler.partialDemangle(function->getName().str().c_str()) &&
         demangler.isCtorOrDtor() &&
         demangleUnqualifiedName(function->getName()).find('~') !=
             std::string::npos;
}

inline const char *branchConditionFromTo(BasicBlock *from, BasicBlock *to) {
  auto *branch = dyn_cast<BranchInst>(from->getTerminator());
  if (!branch || !branch->isConditional())
    return "true";
  return branch->getSuccessor(0) == to ? "true" : "false";
}

inline bool isSyntheticLoopTrap(const Instruction *instruction) {
  return instruction->getMetadata("lotus.loop-summary.trap") != nullptr;
}

} // namespace gsaf
} // namespace lotus
