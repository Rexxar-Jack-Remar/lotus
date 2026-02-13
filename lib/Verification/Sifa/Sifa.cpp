// Sifa: Symbolic Interpretation with Fluid Abstractions.
// Algorithm from TACAS 2020 "Ultimate Taipan with Symbolic Interpretation and
// Fluid Abstractions" (Dietsch et al.) — ICFG/DAG interpreters, RegexDAG,
// post operator, call/loop summarization, fluid abstraction policies.
#include "Verification/Sifa/Sifa.h"

#include "Verification/Sifa/Domain/EqDomain.h"
#include "Verification/Sifa/Domain/ExplicitValueDomain.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/OctagonDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Interpreter/IcfgInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

using namespace lotus::sifa;

bool lotus::sifa::isReachable(const llvm::Function &F, const llvm::BasicBlock &target,
                              SifaOptions options) {
  // Current reachability pipeline is self-contained and does not yet consult:
  // - options.blockTransferPolicy (value-domain only),
  // - options.aliasAnalysis (value-domain only),
  // - options.logLevel (logger integration is optional).
  (void)options;

  SifaStats stats;
  ReachabilityDomain<Transition> domain;
  NeverFluid<bool> fluid;

  // Interpreter stack (Ultimate-aligned):
  // - build ProcedureResources: path expressions -> regex -> regex DAG + overlays
  // - interpret the DAG with optional loop summarization
  DagInterpreter<Transition, bool> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, bool> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  const ProcedureResources res(stats, F, {const_cast<llvm::BasicBlock *>(&target)});
  const bool out = ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(), /*in=*/true);
  return out;
}

bool lotus::sifa::isReachableInterprocedural(const llvm::Module &M, const llvm::Function *entry,
                                             const llvm::Function &targetFunc,
                                             const llvm::BasicBlock &targetBlock,
                                             const SifaOptions &options) {
  // Interprocedural reachability uses IcfgInterpreter and a storage mapping
  // basic blocks to reachability booleans. Options are currently unused here.
  (void)options;

  SifaStats stats;
  ReachabilityDomain<Transition> domain;
  NeverFluid<bool> fluid;

  std::vector<std::pair<const llvm::Function *, const llvm::BasicBlock *>> lois = {
      {&targetFunc, &targetBlock}};
  IcfgInterpreter<bool> icfg(M, entry, lois, stats, domain, fluid, /*initialState=*/true);
  MapBasedStorage<const llvm::BasicBlock *, bool> storage;
  icfg.interpret(storage);
  auto *bb = const_cast<llvm::BasicBlock *>(&targetBlock);
  auto it = storage.getMap().find(bb);
  return it != storage.getMap().end() && it->second;
}

IntervalState lotus::sifa::analyzeToWithIntervalDomain(const llvm::Function &F,
                                                      const llvm::BasicBlock &target,
                                                      const IntervalState &initial,
                                                      SifaOptions options) {
  IntervalDomain domain(
      options.blockTransferPolicy.hasValue() ? &*options.blockTransferPolicy
                                             : nullptr,
      options.aliasAnalysis);
  return analyzeTo<IntervalState>(F, target, initial, domain, options);
}

OctagonState lotus::sifa::analyzeToWithOctagonDomain(const llvm::Function &F,
                                                    const llvm::BasicBlock &target,
                                                    const OctagonState &initial,
                                                    SifaOptions options) {
  OctagonDomain domain(
      options.blockTransferPolicy.hasValue() ? &*options.blockTransferPolicy
                                             : nullptr,
      options.aliasAnalysis);
  return analyzeTo<OctagonState>(F, target, initial, domain, options);
}

EqState lotus::sifa::analyzeToWithEqDomain(const llvm::Function &F,
                                           const llvm::BasicBlock &target,
                                           const EqState &initial,
                                           SifaOptions options) {
  EqDomain domain(options.blockTransferPolicy.hasValue()
                      ? &*options.blockTransferPolicy
                      : nullptr,
                  options.aliasAnalysis);
  return analyzeTo<EqState>(F, target, initial, domain, options);
}

ExplicitValueState lotus::sifa::analyzeToWithExplicitValueDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const ExplicitValueState &initial, SifaOptions options) {
  ExplicitValueDomain domain(options.blockTransferPolicy.hasValue()
                                  ? &*options.blockTransferPolicy
                                  : nullptr,
                              options.aliasAnalysis);
  return analyzeTo<ExplicitValueState>(F, target, initial, domain, options);
}

static IntervalState runToReturnWithIntervalDomain(const llvm::Function &F,
                                                   const IntervalState &initial,
                                                   SifaOptions options) {
  SifaStats stats;
  IntervalDomain domain(
      options.blockTransferPolicy.hasValue() ? &*options.blockTransferPolicy
                                             : nullptr,
      options.aliasAnalysis);
  NeverFluid<IntervalState> fluid;
  DagInterpreter<Transition, IntervalState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, IntervalState> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);
  ProcedureResources res(stats, F, std::vector<llvm::BasicBlock *>{});
  IntervalState entryState = initial.isBottom() ? domain.top() : initial;
  return ipr.interpretForSingleMarker(res.getRegexDag(),
                                      res.getDagOverlayPathToReturn(), entryState);
}

IntervalState lotus::sifa::analyzeToReturnWithIntervalDomain(const llvm::Function &F,
                                                             const IntervalState &initial,
                                                             SifaOptions options) {
  return runToReturnWithIntervalDomain(F, initial, options);
}

static OctagonState runToReturnWithOctagonDomain(const llvm::Function &F,
                                                 const OctagonState &initial,
                                                 SifaOptions options) {
  SifaStats stats;
  OctagonDomain domain(
      options.blockTransferPolicy.hasValue() ? &*options.blockTransferPolicy
                                             : nullptr,
      options.aliasAnalysis);
  NeverFluid<OctagonState> fluid;
  DagInterpreter<Transition, OctagonState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, OctagonState> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);
  ProcedureResources res(stats, F, std::vector<llvm::BasicBlock *>{});
  OctagonState entryState = initial.isBottom() ? domain.top() : initial;
  return ipr.interpretForSingleMarker(res.getRegexDag(),
                                      res.getDagOverlayPathToReturn(), entryState);
}

OctagonState lotus::sifa::analyzeToReturnWithOctagonDomain(const llvm::Function &F,
                                                          const OctagonState &initial,
                                                          SifaOptions options) {
  return runToReturnWithOctagonDomain(F, initial, options);
}
