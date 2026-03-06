/*
 *
 * Author: rainoftime
 */
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/SaturationProcess.h"
#ifdef WITNESS_TRACE
#include "Solvers/WPDS/Witness.h"
#endif
#include "llvm/ADT/Optional.h"

#include <sstream>
#include <unordered_map>

namespace wpds {

using namespace wpds;
using namespace llvm;

static bool isValueInInstructionScope(Value *v, const Function *f) {
  if (v == nullptr) {
    return false;
  }
  if (isa<GlobalValue>(v) || isa<Constant>(v)) {
    return true;
  }
  if (const auto *a = dyn_cast<Argument>(v)) {
    return a->getParent() == f;
  }
  if (const auto *i = dyn_cast<Instruction>(v)) {
    return i->getFunction() == f;
  }
  return true;
}

static void filterFactsToInstructionScope(Instruction *inst,
                                          std::set<Value *> &facts) {
  if (inst == nullptr) {
    facts.clear();
    return;
  }
  Function *f = inst->getFunction();
  if (f == nullptr) {
    return;
  }
  for (auto it = facts.begin(); it != facts.end();) {
    if (!isValueInInstructionScope(*it, f)) {
      it = facts.erase(it);
    } else {
      ++it;
    }
  }
}

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine()
    : controlState(str2key("q")) {}

void InterProceduralDataFlowEngine::setCalleeResolver(CalleeResolver resolver) {
  calleeResolver = std::move(resolver);
}

void InterProceduralDataFlowEngine::setExternalCallPolicy(
    ExternalCallPolicy policy) {
  externalCallPolicy = std::move(policy);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts) {
  return runForwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildInitialAutomaton(m, ca, initialFacts, true);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts) {
  return runBackwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildInitialAutomaton(m, ca, initialFacts, false);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA) {
  return runAnalysisWithAutomaton(m, createTransformer, buildInitialCA, true);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysisFromEntries(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::vector<Function *> &entryFunctions,
    const std::set<Value *> &initialFacts) {
  return runForwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildSeedAutomatonForFunctions(ca, entryFunctions, initialFacts, false);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA) {
  return runAnalysisWithAutomaton(m, createTransformer, buildInitialCA, false);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysisFromExits(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::vector<Function *> &exitFunctions,
    const std::set<Value *> &initialFacts) {
  return runBackwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildSeedAutomatonForFunctions(ca, exitFunctions, initialFacts, true);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA, bool isForward) {
  // Create semiring and WPDS
  Semiring<GenKillTransformer> semiring(GenKillTransformer::one(), isForward);
  WPDS<GenKillTransformer> wpds(semiring, isForward ? Query::poststar()
                                                    : Query::prestar());

  // Build WPDS from LLVM module
  buildWPDS(m, wpds, createTransformer);

  // Build initial configuration automaton (in-place)
  CA<GenKillTransformer> resultCA(semiring);
  lastAcceptState = llvm::None;
  buildInitialCA(resultCA);

  // Run saturation algorithm
  wpds::SaturationProcess<GenKillTransformer> satProcess(
      wpds, resultCA, semiring,
      isForward ? Query::poststar() : Query::prestar());
  if (isForward) {
    satProcess.poststar();
  } else {
    satProcess.prestar();
  }

  // Extract results
  currentResult = std::make_unique<mono::DataFlowResult>();
  extractResults(m, resultCA, currentResult, isForward);

  // Cache for queries/witnesses
  lastResultCA = std::make_unique<CA<GenKillTransformer>>(resultCA);
  lastQuery = isForward ? Query::poststar() : Query::prestar();

  return std::make_unique<mono::DataFlowResult>(*currentResult);
}

const std::set<Value *> &
InterProceduralDataFlowEngine::getInSet(Instruction *inst) const {
  if (!currentResult) {
    static std::set<Value *> emptySet;
    return emptySet;
  }
  return currentResult->IN(inst);
}

const std::set<Value *> &
InterProceduralDataFlowEngine::getOutSet(Instruction *inst) const {
  if (!currentResult) {
    static std::set<Value *> emptySet;
    return emptySet;
  }
  return currentResult->OUT(inst);
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsBeforeInstruction(
    Instruction *inst) const {
  return queryFactsAtSymbol(getProgramPointKeyBeforeInstruction(inst));
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsAfterInstruction(
    Instruction *inst) const {
  return queryFactsAtSymbol(getProgramPointKeyAfterInstruction(inst));
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryBeforeInstruction(
    Instruction *inst) const {
  return querySummaryAtSymbol(getProgramPointKeyBeforeInstruction(inst));
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAfterInstruction(
    Instruction *inst) const {
  return querySummaryAtSymbol(getProgramPointKeyAfterInstruction(inst));
}

wpds::wpds_key_t
InterProceduralDataFlowEngine::getProgramPointKeyBeforeInstruction(
    Instruction *inst) const {
  auto it = instPrevKey.find(inst);
  return it != instPrevKey.end() ? it->second : WPDS_EPSILON;
}

wpds::wpds_key_t
InterProceduralDataFlowEngine::getProgramPointKeyAfterInstruction(
    Instruction *inst) const {
  auto it = instToKey.find(inst);
  return it != instToKey.end() ? it->second : WPDS_EPSILON;
}

void InterProceduralDataFlowEngine::buildWPDS(
    Module &m, WPDS<GenKillTransformer> &wpds,
    const std::function<GenKillTransformer *(Instruction *)>
        &createTransformer) {
  ::dataflow::controlflow::LLVMIntraCFG intraCfg;
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> interCfgStorage;
  if (calleeResolver) {
    interCfgStorage = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        &m, [this](Instruction *inst) -> std::vector<Function *> {
          auto *call = dyn_cast<CallBase>(inst);
          return call ? calleeResolver(call) : std::vector<Function *>{};
        });
  } else {
    interCfgStorage =
        std::make_unique<::dataflow::controlflow::LLVMInterCFG>(&m);
  }
  auto &interCfg = *interCfgStorage;

  // Clear previous mappings
  functionToKey.clear();
  functionExitToKey.clear();
  instToKey.clear();
  instPrevKey.clear();
  bbToKey.clear();
  callReturnToKey.clear();
  keyToInst.clear();
  localGenByInst.clear();
  localKillByInst.clear();

  auto functionTag = [&](Function &F) -> std::string {
    std::string fname = F.getName().str();
    if (fname.empty()) {
      fname = "anon";
    }
    return fname + "_" + std::to_string((uintptr_t)&F);
  };

  auto bbTag = [&](Function &F, BasicBlock &BB) -> std::string {
    std::string name = BB.getName().str();
    if (name.empty()) {
      name = "bb";
    }
    return functionTag(F) + "_bb_" + name + "_" +
           std::to_string((uintptr_t)&BB);
  };

  auto instTag = [&](Function &F, Instruction &I) -> std::string {
    std::string name = I.getName().str();
    if (name.empty()) {
      name = "inst";
    }
    return functionTag(F) + "_i_" + name + "_" + std::to_string((uintptr_t)&I);
  };

  auto retTag = [&](Function &F, Instruction &callI) -> std::string {
    return functionTag(F) + "_ret_" + std::to_string((uintptr_t)&callI);
  };

  auto ensureExitPopRule = [&](wpds_key_t exitKey) {
    // Pop the callee-exit symbol to reveal the return-site symbol below.
    // Safe to add redundantly; WPDS will combine/ignore duplicates.
    wpds.add_rule(controlState, exitKey, controlState,
                  GenKillTransformer::one());
  };

  // First pass: Create function entry and exit keys for all functions
  for (auto &F : m) {
    if (F.isDeclaration())
      continue;

    const std::string ftag = functionTag(F);
    wpds_key_t funcEntry = new_str2key(("entry_" + ftag).c_str());
    wpds_key_t funcExit = new_str2key(("exit_" + ftag).c_str());
    functionToKey[&F] = funcEntry;
    functionExitToKey[&F] = funcExit;

    wpds.add_element_to_P(controlState);
  }

  // Second pass: Create rules for each function
  for (auto &F : m) {
    if (F.isDeclaration())
      continue;

    wpds_key_t funcEntry = functionToKey[&F];
    wpds_key_t funcExit = functionExitToKey[&F];

    // Map basic blocks to keys
    for (auto &BB : F) {
      const std::string btag = bbTag(F, BB);
      wpds_key_t bbKey = new_str2key(btag.c_str());
      bbToKey[&BB] = bbKey;
    }

    // Rule from function entry to first basic block
    BasicBlock &entryBB = F.getEntryBlock();
    wpds_key_t entryBBKey = bbToKey[&entryBB];
    wpds.add_rule(controlState, funcEntry, controlState, entryBBKey,
                  GenKillTransformer::one());

    // Track return-site transformers so we can compose them at the return site.
    std::unordered_map<wpds_key_t, ::ref_ptr<GenKillTransformer>>
        returnSiteTransformers;

    // Process each basic block
    for (auto &BB : F) {
      wpds_key_t bbKey = bbToKey[&BB];

      wpds_key_t prevKey = bbKey;

      // Process instructions in the basic block
      for (auto &I : BB) {
        // Create instruction key
        const std::string itag = instTag(F, I);
        wpds_key_t instKey = new_str2key(itag.c_str());
        instToKey[&I] = instKey;
        keyToInst[instKey] = &I;
        instPrevKey[&I] = prevKey;

        // Create transformer for this instruction
        GenKillTransformer *transformer = createTransformer(&I);
        if (!transformer) {
          transformer = GenKillTransformer::one();
        }
        localGenByInst[&I] = transformer->getGen().getFacts();
        localKillByInst[&I] = transformer->getKill().getFacts();

        // If we're returning from a callsite, compose return-flow before this
        // instruction.
        auto retIt = returnSiteTransformers.find(prevKey);
        if (retIt != returnSiteTransformers.end()) {
          transformer = retIt->second.get_ptr()->extend(transformer);
        }

        // Add rule from previous location to this instruction
        wpds.add_rule(controlState, prevKey, controlState, instKey,
                      transformer);

        // Handle different instruction types
        if (auto *callInst = dyn_cast<CallBase>(&I)) {
          std::vector<Function *> callees;
          if (calleeResolver) {
            callees = calleeResolver(callInst);
          } else {
            callees = interCfg.getCalleesOfCallAt(callInst);
          }

          bool hasModeledCallee = false;
          bool hasUnmodeledCallee = callees.empty();

          // Create a single, callsite-specific return symbol to represent the
          // return site. (All possible callees return to the same point.)
          llvm::Optional<wpds_key_t> returnKeyOpt;
          auto getReturnKey = [&]() -> wpds_key_t {
            if (returnKeyOpt.hasValue()) {
              return *returnKeyOpt;
            }
            wpds_key_t rk = new_str2key(retTag(F, I).c_str());
            returnKeyOpt = rk;
            callReturnToKey[callInst] = rk;
            return rk;
          };

          for (Function *calledFunc : callees) {
            if (!calledFunc || calledFunc->isDeclaration() ||
                functionToKey.find(calledFunc) == functionToKey.end()) {
              hasUnmodeledCallee = true;
              continue;
            }

            hasModeledCallee = true;

            // Interprocedural call: <q, instKey> -> <q, calledEntry, returnKey>
            wpds_key_t calledEntry = functionToKey[calledFunc];
            wpds_key_t calledExit = functionExitToKey[calledFunc];
            wpds_key_t returnKey = getReturnKey();

            // Call rule weight maps actuals to formals.
            std::map<Value *, DataFlowFacts> paramFlow;
            unsigned argIdx = 0;
            for (auto &formal : calledFunc->args()) {
              if (argIdx < callInst->arg_size()) {
                Value *actual = callInst->getArgOperand(argIdx);
                if (!paramFlow.count(actual)) {
                  paramFlow[actual] = DataFlowFacts::EmptySet();
                }
                paramFlow[actual].addFact(&formal);
              }
              argIdx++;
            }
            GenKillTransformer *callTrans =
                GenKillTransformer::makeGenKillTransformer(
                    DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                    paramFlow);

            wpds.add_rule(controlState, instKey, controlState, calledEntry,
                          returnKey, callTrans);

            // Return-flow mapping: any returned SSA value in the callee can
            // flow to the call result.
            std::map<Value *, DataFlowFacts> retFlow;
            if (!callInst->getType()->isVoidTy()) {
              for (auto &calleeBB : *calledFunc) {
                if (auto *RI = dyn_cast<ReturnInst>(calleeBB.getTerminator())) {
                  Value *rv = RI->getReturnValue();
                  if (rv == nullptr) {
                    continue;
                  }
                  if (!retFlow.count(rv)) {
                    retFlow[rv] = DataFlowFacts::EmptySet();
                  }
                  retFlow[rv].addFact(callInst);
                }
              }
            }
            GenKillTransformer *retTrans =
                GenKillTransformer::makeGenKillTransformer(
                    DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                    retFlow);

            // Ensure the standard exit-pop rule exists for the callee.
            ensureExitPopRule(calledExit);

            // Apply return-flow at the return site before the next instruction.
            auto existing = returnSiteTransformers.find(returnKey);
            if (existing == returnSiteTransformers.end()) {
              returnSiteTransformers[returnKey] =
                  ::ref_ptr<GenKillTransformer>(retTrans);
            } else {
              returnSiteTransformers[returnKey] = ::ref_ptr<GenKillTransformer>(
                  existing->second.get_ptr()->combine(retTrans));
            }
          }

          // If the call has any modeled callee, we need an explicit return-site
          // symbol. Also, for mixed/unknown callees, add a conservative direct
          // edge to the return site.
          if (hasModeledCallee) {
            wpds_key_t returnKey = getReturnKey();
            if (hasUnmodeledCallee) {
              wpds.add_rule(controlState, instKey, controlState, returnKey,
                            buildUnknownCallSummary(callInst, m));
            }

            // For terminator calls (e.g., invoke/callbr), connect return-site
            // continuations through CFG to successor basic blocks. For
            // unmodeled calls we fall back to instKey as the continuation
            // point.
            if (I.isTerminator()) {
              auto retIt = returnSiteTransformers.find(returnKey);
              GenKillTransformer *retWeight =
                  (retIt != returnSiteTransformers.end() &&
                   retIt->second.get_ptr())
                      ? retIt->second.get_ptr()
                      : GenKillTransformer::one();
              for (auto *retSite : interCfg.getReturnSitesOfCallAt(callInst)) {
                if (retSite == nullptr) {
                  continue;
                }
                auto *retBB = retSite->getParent();
                auto bbIt = bbToKey.find(retBB);
                if (bbIt == bbToKey.end()) {
                  continue;
                }
                wpds.add_rule(controlState, returnKey, controlState,
                              bbIt->second, retWeight);
              }
            }

            prevKey = returnKey;
            continue;
          }

          // No modeled callee: use a synthetic return-site symbol so the
          // external-call summary is applied exactly once before continuing.
          if (hasUnmodeledCallee) {
            wpds_key_t returnKey = getReturnKey();
            wpds.add_rule(controlState, instKey, controlState, returnKey,
                          buildUnknownCallSummary(callInst, m));

            if (I.isTerminator()) {
              for (auto *retSite : interCfg.getReturnSitesOfCallAt(callInst)) {
                if (retSite == nullptr) {
                  continue;
                }
                auto *retBB = retSite->getParent();
                auto bbIt = bbToKey.find(retBB);
                if (bbIt == bbToKey.end()) {
                  continue;
                }
                wpds.add_rule(controlState, returnKey, controlState,
                              bbIt->second, GenKillTransformer::one());
              }
            }

            prevKey = returnKey;
            continue;
          }
        }

        if (isa<ReturnInst>(&I)) {
          // Return: <q, instKey> -> <q, funcExit>
          wpds.add_rule(controlState, instKey, controlState, funcExit,
                        GenKillTransformer::one());
          prevKey = instKey;
          continue;
        }

        // Regular instruction - continue to next
        prevKey = instKey;
      }

      // Connect terminator to successor basic blocks
      if (Instruction *terminator = BB.getTerminator()) {
        wpds_key_t termKey = instToKey[terminator];

        // If terminator is not a return or call, connect to successors
        if (!isa<ReturnInst>(terminator) && !isa<CallBase>(terminator)) {
          for (auto *succInst : intraCfg.getSuccsOf(
                   terminator,
                   ::dataflow::controlflow::FlowDirection::Forward)) {
            if (succInst == nullptr) {
              continue;
            }
            auto *succBB = succInst->getParent();
            wpds_key_t succBBKey = bbToKey[succBB];
            wpds.add_rule(controlState, termKey, controlState, succBBKey,
                          GenKillTransformer::one());
          }
        }
      }
    }
  }
}

void InterProceduralDataFlowEngine::buildInitialAutomaton(
    Module &m, CA<GenKillTransformer> &ca,
    const std::set<Value *> &initialFacts, bool isForward) {

  wpds_key_t acceptState = str2key("accept");
  lastAcceptState = acceptState;

  ca.add_initial_state(controlState);
  ca.add_final_state(acceptState);

  if (isForward) {
    // For forward analysis: start from main if present, otherwise seed all
    // entries.
    Function *mainFn = nullptr;
    for (auto &F : m) {
      if (F.isDeclaration())
        continue;
      if (F.getName() == "main") {
        mainFn = &F;
        break;
      }
    }
    GenKillTransformer *initTrans = GenKillTransformer::makeGenKillTransformer(
        DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));
    if (mainFn) {
      ca.add(controlState, functionToKey[mainFn], acceptState, initTrans);
    } else {
      for (auto &kv : functionToKey) {
        ca.add(controlState, kv.second, acceptState, initTrans);
      }
    }
  } else {
    // For backward analysis: start from all exit points
    for (auto &kv : functionExitToKey) {
      wpds_key_t exitKey = kv.second;

      GenKillTransformer *initTrans =
          GenKillTransformer::makeGenKillTransformer(
              DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));

      ca.add(controlState, exitKey, acceptState, initTrans);
    }
  }
}

void InterProceduralDataFlowEngine::buildSeedAutomatonForFunctions(
    CA<GenKillTransformer> &ca, const std::vector<Function *> &functions,
    const std::set<Value *> &initialFacts, bool useExitPoints) {
  wpds_key_t acceptState = str2key("accept");
  lastAcceptState = acceptState;

  ca.add_initial_state(controlState);
  ca.add_final_state(acceptState);

  GenKillTransformer *initTrans = GenKillTransformer::makeGenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));

  for (Function *function : functions) {
    if (function == nullptr) {
      continue;
    }
    auto &keyMap = useExitPoints ? functionExitToKey : functionToKey;
    auto it = keyMap.find(function);
    if (it == keyMap.end()) {
      continue;
    }
    ca.add(controlState, it->second, acceptState, initTrans);
  }
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForFunction(Function *f) {
  auto it = functionToKey.find(f);
  if (it != functionToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForInstruction(Instruction *inst) {
  auto it = instToKey.find(inst);
  if (it != instToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForBasicBlock(BasicBlock *bb) {
  auto it = bbToKey.find(bb);
  if (it != bbToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForCallSite(CallBase *callInst) {
  return getKeyForInstruction(callInst);
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForReturnSite(CallBase *callInst) {
  auto it = callReturnToKey.find(callInst);
  if (it != callReturnToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

void InterProceduralDataFlowEngine::extractResults(
    Module &m, CA<GenKillTransformer> &resultCA,
    std::unique_ptr<mono::DataFlowResult> &result, bool isForward) {
  (void)m;
  (void)isForward;

  wpds_key_t queryInit = resultCA.initial_state();
  if (queryInit == WPDS_EPSILON) {
    queryInit = controlState;
  }

  // Cache value-at-symbol queries to avoid repeated reglang_query work.
  struct KeyQueryResult {
    std::set<Value *> facts;
    llvm::Optional<std::set<Value *>> gen;
    llvm::Optional<std::set<Value *>> kill;
  };
  std::unordered_map<wpds_key_t, KeyQueryResult> cache;

  auto querySymbol = [&](wpds_key_t sym,
                         bool wantGenKill) -> const KeyQueryResult & {
    auto it = cache.find(sym);
    if (it != cache.end() && (!wantGenKill || (it->second.gen.hasValue() &&
                                               it->second.kill.hasValue()))) {
      return it->second;
    }

    // Query the regular language consisting of the single stack symbol `sym`.
    CA<GenKillTransformer> lang(resultCA.semiring());
    wpds_key_t qf =
        new_str2key(("query_final_" + std::to_string((uintptr_t)sym)).c_str());
    lang.add_initial_state(queryInit);
    lang.add_final_state(qf);
    lang.add(queryInit, sym, qf, GenKillTransformer::one());

    auto pathSummary = resultCA.reglang_query(lang);
    KeyQueryResult res;
    // Treat zero (no path / empty intersection) explicitly so we don't rely on
    // zero()->apply semantics.
    if (pathSummary.get_ptr() &&
        !pathSummary->equal(GenKillTransformer::zero())) {
      DataFlowFacts outFacts = pathSummary->apply(DataFlowFacts::EmptySet());
      res.facts = outFacts.getFacts();
      if (wantGenKill) {
        res.gen = pathSummary->getGen().getFacts();
        res.kill = pathSummary->getKill().getFacts();
      }
    }

    if (it == cache.end()) {
      cache.emplace(sym, std::move(res));
      return cache.find(sym)->second;
    }
    it->second = std::move(res);
    return it->second;
  };

  // Compute IN/OUT directly from the saturated automaton.
  for (auto &kv : instToKey) {
    Instruction *inst = kv.first;
    wpds_key_t instKey = kv.second;

    // OUT at instruction = value at the "after-inst" program-point symbol.
    result->OUT(inst) = querySymbol(instKey, /*wantGenKill=*/true).facts;
    filterFactsToInstructionScope(inst, result->OUT(inst));

    // IN at instruction = value at the program-point symbol that precedes the
    // instruction.
    auto pkIt = instPrevKey.find(inst);
    if (pkIt != instPrevKey.end()) {
      result->IN(inst) = querySymbol(pkIt->second, /*wantGenKill=*/false).facts;
      filterFactsToInstructionScope(inst, result->IN(inst));
    }

    auto genIt = localGenByInst.find(inst);
    if (genIt != localGenByInst.end()) {
      result->GEN(inst) = genIt->second;
      filterFactsToInstructionScope(inst, result->GEN(inst));
    }
    auto killIt = localKillByInst.find(inst);
    if (killIt != localKillByInst.end()) {
      result->KILL(inst) = killIt->second;
      filterFactsToInstructionScope(inst, result->KILL(inst));
    }
  }
}

const wpds::CA<GenKillTransformer> *
InterProceduralDataFlowEngine::getLastResultAutomaton() const {
  return lastResultCA.get();
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAtSymbol(wpds::wpds_key_t symbol) const {
  if (!lastResultCA || symbol == WPDS_EPSILON) {
    return ::ref_ptr<GenKillTransformer>(GenKillTransformer::zero());
  }

  wpds_key_t queryInit = lastResultCA->initial_state();
  if (queryInit == WPDS_EPSILON) {
    queryInit = controlState;
  }

  CA<GenKillTransformer> lang(lastResultCA->semiring());
  wpds_key_t qf =
      new_str2key(("query_final_" + std::to_string((uintptr_t)symbol)).c_str());
  lang.add_initial_state(queryInit);
  lang.add_final_state(qf);
  lang.add(queryInit, symbol, qf, GenKillTransformer::one());
  return lastResultCA->reglang_query(lang);
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsAtSymbol(wpds::wpds_key_t symbol) const {
  auto summary = querySummaryAtSymbol(symbol);
  if (!summary.get_ptr() || summary->equal(GenKillTransformer::zero())) {
    return {};
  }
  DataFlowFacts facts = summary->apply(DataFlowFacts::EmptySet());
  return facts.getFacts();
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::queryRegularLanguage(
    const wpds::CA<GenKillTransformer> &lang) const {
  if (!lastResultCA) {
    return ::ref_ptr<GenKillTransformer>(GenKillTransformer::zero());
  }
  return lastResultCA->reglang_query(lang);
}

GenKillTransformer *InterProceduralDataFlowEngine::buildUnknownCallSummary(
    CallBase *callInst, Module &m) const {
  if (callInst == nullptr) {
    return GenKillTransformer::one();
  }

  std::vector<Value *> pointerObjects =
      MemoryObjectFact::pointerArgumentObjects(callInst);
  std::vector<GlobalValue *> globals = MemoryObjectFact::trackedGlobals(m);

  if (externalCallPolicy.buildSummary) {
    if (GenKillTransformer *custom =
            externalCallPolicy.buildSummary(callInst, pointerObjects, globals)) {
      return custom;
    }
  }

  if (!externalCallPolicy.preserveIdentity &&
      !externalCallPolicy.flowPointerArgumentsToReturn &&
      !externalCallPolicy.flowGlobalsToReturn) {
    return GenKillTransformer::zero();
  }

  std::map<Value *, DataFlowFacts> flow;
  if (!callInst->getType()->isVoidTy()) {
    if (externalCallPolicy.flowPointerArgumentsToReturn) {
      for (Value *object : pointerObjects) {
        MemoryObjectFact::addFlow(flow, object, callInst);
      }
    }
    if (externalCallPolicy.flowGlobalsToReturn) {
      for (GlobalValue *global : globals) {
        MemoryObjectFact::addFlow(flow, global, callInst);
      }
    }
  }

  return GenKillTransformer::makeGenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(), flow);
}

#ifdef WITNESS_TRACE
std::string InterProceduralDataFlowEngine::getWitnessDagDotForTransition(
    wpds::wpds_key_t from, wpds::wpds_key_t stack, wpds::wpds_key_t to) const {
  if (!lastResultCA) {
    return "";
  }
  wpds::CA<GenKillTransformer>::catrans_t trans;
  if (!lastResultCA->find(from, stack, to, trans) || !trans.get_ptr()) {
    return "";
  }
  auto wit = trans->witness();
  if (!wit.get_ptr()) {
    return "";
  }

  using witness_path_t =
      wpds::ref_ptr<wpds::CAPathOfWitness<GenKillTransformer>>;
  witness_path_t path(
      new wpds::CAPathOfWitness<GenKillTransformer>(wit, witness_path_t(0)));
  auto dag =
      wpds::DAGWitnessForPath<GenKillTransformer>::createFromCAPathOfWitness(
          path, lastQuery);
  std::ostringstream oss;
  dag->print(oss);
  return oss.str();
}

std::string InterProceduralDataFlowEngine::getWitnessDagDotForInstruction(
    Instruction *inst) const {
  if (!lastAcceptState.hasValue()) {
    return "";
  }
  auto it = instToKey.find(inst);
  if (it == instToKey.end()) {
    return "";
  }
  return getWitnessDagDotForTransition(controlState, it->second,
                                       *lastAcceptState);
}
#endif

} // namespace wpds
