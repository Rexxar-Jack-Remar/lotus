#include "Verification/Sifa/SifaSymAbs.h"

#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

#include "Verification/SymbolicAbstraction/Analyzers/Analyzer.h"
#include "Verification/SymbolicAbstraction/Core/DomainConstructor.h"
#include "Verification/SymbolicAbstraction/Core/FragmentDecomposition.h"
#include "Verification/SymbolicAbstraction/Core/FunctionContext.h"
#include "Verification/SymbolicAbstraction/Core/ModuleContext.h"
#include "Verification/SymbolicAbstraction/Utils/Config.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <vector>

using namespace lotus::sifa;

static symbolic_abstraction::configparser::Config
makeConfig(const SifaSymAbsOptions &opt) {
  symbolic_abstraction::configparser::Config cfg;
  cfg.set("ModuleContext", "Recursive", opt.recursive);
  cfg.set("Analyzer", "Variant", opt.analyzerVariant);
  cfg.set("AbstractDomain", "Variant", opt.abstractDomain);
  return cfg;
}

static SymAbsState runForTarget(const llvm::Module &M, const llvm::Function &F,
                                llvm::BasicBlock *target,
                                const SifaSymAbsOptions &options) {
  auto cfg = makeConfig(options);

  // SymbolicAbstraction expects non-const pointers (it mutates analysis state
  // and queries IR properties through non-const APIs).
  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  symbolic_abstraction::ModuleContext mctx(mod, cfg);
  auto fctxPtr = mctx.createFunctionContext(fun);
  auto fragDecomp = symbolic_abstraction::FragmentDecomposition::For(*fctxPtr);
  const auto fcfg = fctxPtr->getConfig();
  symbolic_abstraction::DomainConstructor dom(fcfg);
  auto analyzer = symbolic_abstraction::Analyzer::New(*fctxPtr, fragDecomp, dom);

  SifaStats stats;
  SifaSymAbsDomain domain(*fctxPtr, dom, *analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  ProcedureResources res(stats, *fun, {target});
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  // Interpret for the unique marker in the LOI overlay.
  SymAbsState out =
      ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(), initial);
  return out;
}

static SymAbsState runForReturn(const llvm::Module &M, const llvm::Function &F,
                                const SifaSymAbsOptions &options) {
  auto cfg = makeConfig(options);

  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  symbolic_abstraction::ModuleContext mctx(mod, cfg);
  auto fctxPtr = mctx.createFunctionContext(fun);
  auto fragDecomp = symbolic_abstraction::FragmentDecomposition::For(*fctxPtr);
  const auto fcfg = fctxPtr->getConfig();
  symbolic_abstraction::DomainConstructor dom(fcfg);
  auto analyzer = symbolic_abstraction::Analyzer::New(*fctxPtr, fragDecomp, dom);

  SifaStats stats;
  SifaSymAbsDomain domain(*fctxPtr, dom, *analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  // No LOIs needed; ProcedureResources always adds an EXIT marker.
  ProcedureResources res(stats, *fun, std::vector<llvm::BasicBlock *>{});
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  return ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToReturn(), initial);
}

SymAbsState lotus::sifa::analyzeSymAbsTo(const llvm::Module &M, const llvm::Function &F,
                                         const llvm::BasicBlock &target,
                                         const SifaSymAbsOptions &options) {
  return runForTarget(M, F, const_cast<llvm::BasicBlock *>(&target), options);
}

bool lotus::sifa::isReachableSymAbs(const llvm::Module &M, const llvm::Function &F,
                                    const llvm::BasicBlock &target,
                                    const SifaSymAbsOptions &options) {
  const SymAbsState out = analyzeSymAbsTo(M, F, target, options);
  return out && !out->isBottom();
}

SymAbsState lotus::sifa::analyzeSymAbsToReturn(const llvm::Module &M, const llvm::Function &F,
                                               const SifaSymAbsOptions &options) {
  return runForReturn(M, F, options);
}
