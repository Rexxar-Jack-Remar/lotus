/*
 *
 * Author: rainoftime
 */
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/WPDS/Backend/Model.h"
#include "Dataflow/WPDS/Backend/PreparedAnalysisImpl.h"
#include "Dataflow/WPDS/Backend/PreparedBackend.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "WPDS/CA.h"
#include "WPDS/SaturationProcess.h"
#ifdef WITNESS_TRACE
#include "WPDS/Witness.h"
#endif
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <optional>
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

static GenKillTransformer *materializeLegacyWeight(const GenKillValue &value) {
  switch (value.getKind()) {
  case GenKillValue::Kind::One:
    return GenKillTransformer::one();
  case GenKillValue::Kind::Zero:
    return GenKillTransformer::zero();
  case GenKillValue::Kind::Bottom:
    return GenKillTransformer::bottom();
  case GenKillValue::Kind::Ordinary:
    return GenKillTransformer::makeGenKillTransformer(
        value.getKill(), value.getGen(), value.getFlow());
  }
  return GenKillTransformer::zero();
}

namespace {

class EnginePreparedAnalysis final : public backend::PreparedAnalysisImpl {
public:
  EnginePreparedAnalysis(std::unique_ptr<backend::Model> model,
                         std::unique_ptr<backend::PreparedBackend> selected,
                         std::unique_ptr<backend::PreparedBackend> verification,
                         std::vector<backend::StackSymbolId> roots,
                         std::map<Instruction *, std::uint32_t> before,
                         std::map<Instruction *, std::uint32_t> after,
                         std::map<Instruction *, std::set<Value *>> localGen,
                         std::map<Instruction *, std::set<Value *>> localKill,
                         double loweringMilliseconds)
      : model(std::move(model)), selected(std::move(selected)),
        verification(std::move(verification)), roots(std::move(roots)),
        before(std::move(before)), after(std::move(after)),
        localGen(std::move(localGen)), localKill(std::move(localKill)),
        loweringMilliseconds(loweringMilliseconds) {
    sessionStatistics = this->selected->statistics();
    sessionStatistics.loweringMilliseconds = loweringMilliseconds;
  }

  std::unique_ptr<mono::DataFlowResult>
  solve(const std::set<Value *> &initialFacts) override {
    return solveImpl(initialFacts, WPDSObservationKind::ExactStack);
  }

  std::unique_ptr<mono::DataFlowResult>
  solveContextAggregated(const std::set<Value *> &initialFacts) override {
    return solveImpl(initialFacts, WPDSObservationKind::StackPrefix);
  }

  std::unique_ptr<mono::DataFlowResult>
  solveImpl(const std::set<Value *> &initialFacts,
            WPDSObservationKind observation) {
    error.clear();
    backend::Query query;
    query.operation = WPDSQueryKind::PostStar;
    query.observation = observation;
    query.initialState = 0;
    query.roots = roots;
    query.seed = GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                          DataFlowFacts(initialFacts));

    backend::QueryResult result;
    if (!selected->solve(query, result, error)) {
      sessionStatistics = selected->statistics();
      sessionStatistics.loweringMilliseconds = loweringMilliseconds;
      return nullptr;
    }
    if (verification) {
      backend::QueryResult reference;
      std::string verificationError;
      if (!verification->solve(query, reference, verificationError)) {
        error = "legacy verification solve failed: " + verificationError;
        return nullptr;
      }
      for (backend::StackSymbolId symbol = 1;
           symbol < model->stackSymbolNames().size(); ++symbol) {
        const auto actual = result.observations.find(symbol);
        const auto expected = reference.observations.find(symbol);
        const bool actualReachable =
            actual != result.observations.end() && actual->second.reachable;
        const bool expectedReachable =
            expected != reference.observations.end() &&
            expected->second.reachable;
        const bool weightsMatch =
            !actualReachable ||
            (expectedReachable &&
             DataFlowFacts::Eq(
                 actual->second.summary.apply(DataFlowFacts::EmptySet()),
                 expected->second.summary.apply(DataFlowFacts::EmptySet())));
        if (actualReachable != expectedReachable || !weightsMatch) {
          std::ostringstream details;
          details << "WPDS backend verification mismatch at '"
                  << model->stackSymbolName(symbol) << "' for poststar: "
                  << "selected reachable=" << actualReachable;
          if (actualReachable) {
            details << " weight=";
            actual->second.summary.print(details);
          }
          details << ", legacy reachable=" << expectedReachable;
          if (expectedReachable) {
            details << " weight=";
            expected->second.summary.print(details);
          }
          error = details.str();
          return nullptr;
        }
      }
    }

    auto output = std::make_unique<mono::DataFlowResult>();
    auto findObservation =
        [&](std::uint32_t symbol) -> const backend::Observation * {
      auto it = result.observations.find(symbol);
      return it == result.observations.end() ? nullptr : &it->second;
    };
    for (const auto &entry : after) {
      Instruction *instruction = entry.first;
      const backend::Observation *out = findObservation(entry.second);
      if (out != nullptr && out->reachable) {
        output->OUT(instruction) =
            out->summary.apply(DataFlowFacts::EmptySet()).getFacts();
      }
      filterFactsToInstructionScope(instruction, output->OUT(instruction));

      auto beforeIt = before.find(instruction);
      if (beforeIt != before.end()) {
        const backend::Observation *in = findObservation(beforeIt->second);
        if (in != nullptr && in->reachable) {
          output->IN(instruction) =
              in->summary.apply(DataFlowFacts::EmptySet()).getFacts();
        }
        filterFactsToInstructionScope(instruction, output->IN(instruction));
      }
      auto gen = localGen.find(instruction);
      if (gen != localGen.end()) {
        output->GEN(instruction) = gen->second;
        filterFactsToInstructionScope(instruction, output->GEN(instruction));
      }
      auto kill = localKill.find(instruction);
      if (kill != localKill.end()) {
        output->KILL(instruction) = kill->second;
        filterFactsToInstructionScope(instruction, output->KILL(instruction));
      }
    }
    sessionStatistics = selected->statistics();
    sessionStatistics.loweringMilliseconds = loweringMilliseconds;
    return output;
  }

  const WPDSBackendStatistics &statistics() const override {
    return sessionStatistics;
  }

  const std::string &lastError() const override { return error; }

private:
  std::unique_ptr<backend::Model> model;
  std::unique_ptr<backend::PreparedBackend> selected;
  std::unique_ptr<backend::PreparedBackend> verification;
  std::vector<backend::StackSymbolId> roots;
  std::map<Instruction *, std::uint32_t> before;
  std::map<Instruction *, std::uint32_t> after;
  std::map<Instruction *, std::set<Value *>> localGen;
  std::map<Instruction *, std::set<Value *>> localKill;
  double loweringMilliseconds = 0.0;
  WPDSBackendStatistics sessionStatistics;
  std::string error;
};

} // namespace

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine()
    : controlState(str2key("q")) {}

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine(
    WPDSBackendOptions options)
    : backendOptions(options), controlState(str2key("q")) {}

void InterProceduralDataFlowEngine::setBackendOptions(
    WPDSBackendOptions options) {
  backendOptions = options;
}

const WPDSBackendOptions &
InterProceduralDataFlowEngine::getBackendOptions() const {
  return backendOptions;
}

const WPDSBackendStatistics &
InterProceduralDataFlowEngine::getLastBackendStatistics() const {
  return lastBackendStatistics;
}

const std::string &InterProceduralDataFlowEngine::getLastError() const {
  return lastError;
}

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
  return runAnalysis(m, createTransformer, initialFacts, true, {}, false);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts) {
  return runAnalysis(m, createTransformer, initialFacts, false, {}, false);
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
  return runAnalysis(m, createTransformer, initialFacts, true, entryFunctions,
                     true);
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
  return runAnalysis(m, createTransformer, initialFacts, false, exitFunctions,
                     true);
}

std::unique_ptr<PreparedAnalysis>
InterProceduralDataFlowEngine::prepareForwardAnalysis(
    Module &m, const std::function<GenKillTransformer *(Instruction *)>
                   &createTransformer) {
  using Clock = std::chrono::steady_clock;
  lastError.clear();
  if (!isWPDSBackendAvailable(backendOptions.backend)) {
    lastError = getWPDSBackendUnavailableReason(backendOptions.backend);
    return nullptr;
  }
  auto loweringStart = Clock::now();
  std::unique_ptr<backend::Model> model =
      buildModel(m, createTransformer, true);
  const double loweringMilliseconds =
      std::chrono::duration<double, std::milli>(Clock::now() - loweringStart)
          .count();

  std::vector<backend::StackSymbolId> roots;
  Function *mainFunction = m.getFunction("main");
  if (mainFunction != nullptr && !mainFunction->isDeclaration()) {
    roots.push_back(modelFunctionEntry.at(mainFunction));
  } else {
    for (const auto &entry : modelFunctionEntry) {
      roots.push_back(entry.second);
    }
  }
  std::unique_ptr<backend::PreparedBackend> selected = backend::prepareBackend(
      *model, backendOptions, WPDSQueryKind::PostStar, roots, lastError);
  if (!selected) {
    return nullptr;
  }
  std::unique_ptr<backend::PreparedBackend> verification;
  if (backendOptions.verifyAgainstLegacy &&
      backendOptions.backend != WPDSBackendKind::Legacy) {
    WPDSBackendOptions legacyOptions;
    verification = backend::prepareBackend(
        *model, legacyOptions, WPDSQueryKind::PostStar, roots, lastError);
    if (!verification) {
      return nullptr;
    }
  }

  auto implementation = std::make_unique<EnginePreparedAnalysis>(
      std::move(model), std::move(selected), std::move(verification), roots,
      modelInstBefore, modelInstAfter, localGenByInst, localKillByInst,
      loweringMilliseconds);
  return std::unique_ptr<PreparedAnalysis>(
      new PreparedAnalysis(std::move(implementation)));
}

std::unique_ptr<PreparedAnalysis>
InterProceduralDataFlowEngine::prepareBackwardAnalysis(
    Module &m, const std::function<GenKillTransformer *(Instruction *)>
                   &createTransformer) {
  using Clock = std::chrono::steady_clock;
  lastError.clear();
  if (!isWPDSBackendAvailable(backendOptions.backend)) {
    lastError = getWPDSBackendUnavailableReason(backendOptions.backend);
    return nullptr;
  }
  auto loweringStart = Clock::now();
  std::unique_ptr<backend::Model> model =
      buildModel(m, createTransformer, false);
  const double loweringMilliseconds =
      std::chrono::duration<double, std::milli>(Clock::now() - loweringStart)
          .count();

  std::vector<backend::StackSymbolId> roots;
  for (const auto &exit : modelFunctionExit) {
    roots.push_back(exit.second);
  }
  std::unique_ptr<backend::PreparedBackend> selected = backend::prepareBackend(
      *model, backendOptions, WPDSQueryKind::PostStar, roots, lastError);
  if (!selected) {
    return nullptr;
  }
  std::unique_ptr<backend::PreparedBackend> verification;
  if (backendOptions.verifyAgainstLegacy &&
      backendOptions.backend != WPDSBackendKind::Legacy) {
    WPDSBackendOptions legacyOptions;
    verification = backend::prepareBackend(
        *model, legacyOptions, WPDSQueryKind::PostStar, roots, lastError);
    if (!verification) {
      return nullptr;
    }
  }

  auto implementation = std::make_unique<EnginePreparedAnalysis>(
      std::move(model), std::move(selected), std::move(verification), roots,
      modelInstBefore, modelInstAfter, localGenByInst, localKillByInst,
      loweringMilliseconds);
  return std::unique_ptr<PreparedAnalysis>(
      new PreparedAnalysis(std::move(implementation)));
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts, bool isForward,
    const std::vector<Function *> &roots, bool explicitRoots) {
  using Clock = std::chrono::steady_clock;
  lastError.clear();
  lastResultCA.reset();
  lastAcceptState = std::nullopt;
  beforeSummaries.clear();
  afterSummaries.clear();

  if (!isWPDSBackendAvailable(backendOptions.backend)) {
    lastError = getWPDSBackendUnavailableReason(backendOptions.backend);
    currentResult.reset();
    return nullptr;
  }

  auto loweringStart = Clock::now();
  std::unique_ptr<backend::Model> model =
      buildModel(m, createTransformer, isForward);
  const double loweringMilliseconds =
      std::chrono::duration<double, std::milli>(Clock::now() - loweringStart)
          .count();

  backend::Query query;
  std::vector<backend::StackSymbolId> preprocessEntries;
  query.operation = WPDSQueryKind::PostStar;
  query.initialState = 0;
  query.seed = GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                        DataFlowFacts(initialFacts));

  if (explicitRoots) {
    for (Function *function : roots) {
      if (function == nullptr) {
        continue;
      }
      auto &mapping = isForward ? modelFunctionEntry : modelFunctionExit;
      auto it = mapping.find(function);
      if (it == mapping.end()) {
        lastError = "WPDS query root is not a defined function in the model";
        currentResult.reset();
        return nullptr;
      }
      query.roots.push_back(it->second);
      preprocessEntries.push_back(it->second);
    }
  } else if (isForward) {
    Function *mainFunction = m.getFunction("main");
    if (mainFunction != nullptr && !mainFunction->isDeclaration()) {
      query.roots.push_back(modelFunctionEntry.at(mainFunction));
      preprocessEntries.push_back(modelFunctionEntry.at(mainFunction));
    } else {
      for (const auto &entry : modelFunctionEntry) {
        query.roots.push_back(entry.second);
        preprocessEntries.push_back(entry.second);
      }
    }
  } else {
    for (const auto &exit : modelFunctionExit) {
      query.roots.push_back(exit.second);
      preprocessEntries.push_back(exit.second);
    }
  }

  std::unique_ptr<backend::PreparedBackend> prepared = backend::prepareBackend(
      *model, backendOptions, query.operation, preprocessEntries, lastError);
  if (!prepared) {
    currentResult.reset();
    return nullptr;
  }

  backend::QueryResult backendResult;
  if (!prepared->solve(query, backendResult, lastError)) {
    currentResult.reset();
    return nullptr;
  }

  if (backendOptions.verifyAgainstLegacy &&
      backendOptions.backend != WPDSBackendKind::Legacy) {
    WPDSBackendOptions legacyOptions;
    legacyOptions.backend = WPDSBackendKind::Legacy;
    std::string verificationError;
    std::unique_ptr<backend::PreparedBackend> reference =
        backend::prepareBackend(*model, legacyOptions, query.operation,
                                preprocessEntries, verificationError);
    backend::QueryResult referenceResult;
    if (!reference ||
        !reference->solve(query, referenceResult, verificationError)) {
      lastError = "legacy verification solve failed: " + verificationError;
      currentResult.reset();
      return nullptr;
    }
    for (backend::StackSymbolId symbol = 1;
         symbol < model->stackSymbolNames().size(); ++symbol) {
      const auto actual = backendResult.observations.find(symbol);
      const auto expected = referenceResult.observations.find(symbol);
      const bool actualReachable = actual != backendResult.observations.end() &&
                                   actual->second.reachable;
      const bool expectedReachable =
          expected != referenceResult.observations.end() &&
          expected->second.reachable;
      const bool weightsMatch =
          !actualReachable ||
          (expectedReachable &&
           DataFlowFacts::Eq(
               actual->second.summary.apply(DataFlowFacts::EmptySet()),
               expected->second.summary.apply(DataFlowFacts::EmptySet())));
      if (actualReachable != expectedReachable || !weightsMatch) {
        std::ostringstream details;
        details << "WPDS backend verification mismatch at '"
                << model->stackSymbolName(symbol) << "' for "
                << toString(query.operation)
                << ": selected reachable=" << actualReachable;
        if (actualReachable) {
          details << " weight=";
          actual->second.summary.print(details);
        }
        details << ", legacy reachable=" << expectedReachable;
        if (expectedReachable) {
          details << " weight=";
          expected->second.summary.print(details);
        }
        lastError = details.str();
        currentResult.reset();
        return nullptr;
      }
    }
  }

  currentResult = std::make_unique<mono::DataFlowResult>();
  extractModelResults(backendResult, currentResult);
  lastBackendStatistics = prepared->statistics();
  lastBackendStatistics.loweringMilliseconds = loweringMilliseconds;

  if (const auto *legacy = prepared->legacyResultAutomaton()) {
    lastResultCA = std::make_unique<CA<GenKillTransformer>>(*legacy);
    controlState = lastResultCA->initial_state();
    if (lastResultCA->final_states().size() == 1) {
      lastAcceptState = *lastResultCA->final_states().begin();
    }
    for (const auto &entry : modelInstTransfer) {
      instToKey[entry.first] = prepared->legacyKeyForSymbol(entry.second);
    }
    for (const auto &entry : modelInstBefore) {
      instPrevKey[entry.first] = prepared->legacyKeyForSymbol(entry.second);
    }
    for (const auto &entry : modelCallReturn) {
      callReturnToKey[entry.first] = prepared->legacyKeyForSymbol(entry.second);
    }
  }
  lastQuery = Query::poststar();
  return std::make_unique<mono::DataFlowResult>(*currentResult);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA, bool isForward) {
  lastError.clear();
  if (backendOptions.backend != WPDSBackendKind::Legacy) {
    lastError = "caller-provided legacy configuration automata are supported "
                "only by the legacy WPDS backend";
    currentResult.reset();
    return nullptr;
  }

  // Model both directions as forward reachability over direction-specific
  // program graphs. This keeps interprocedural call/return wiring explicit.
  Semiring<GenKillTransformer> semiring(GenKillTransformer::one(), true);
  WPDS<GenKillTransformer> wpds(semiring, Query::poststar());

  // Build WPDS from LLVM module
  buildWPDS(m, wpds, createTransformer, isForward);

  // Build initial configuration automaton (in-place)
  CA<GenKillTransformer> resultCA(semiring);
  lastAcceptState = std::nullopt;
  buildInitialCA(resultCA);

  // Run saturation algorithm
  wpds::SaturationProcess<GenKillTransformer> satProcess(
      wpds, resultCA, semiring, Query::poststar());
  satProcess.poststar();

  // Extract results
  currentResult = std::make_unique<mono::DataFlowResult>();
  extractResults(m, resultCA, currentResult, isForward);

  // Cache for queries/witnesses
  lastResultCA = std::make_unique<CA<GenKillTransformer>>(resultCA);
  lastQuery = Query::poststar();

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

std::set<Value *> InterProceduralDataFlowEngine::queryFactsBeforeInstruction(
    Instruction *inst) const {
  if (currentResult) {
    return currentResult->IN(inst);
  }
  std::set<Value *> facts =
      queryFactsAtSymbol(getProgramPointKeyBeforeInstruction(inst));
  filterFactsToInstructionScope(inst, facts);
  return facts;
}

std::set<Value *> InterProceduralDataFlowEngine::queryFactsAfterInstruction(
    Instruction *inst) const {
  if (currentResult) {
    return currentResult->OUT(inst);
  }
  std::set<Value *> facts =
      queryFactsAtSymbol(getProgramPointKeyAfterInstruction(inst));
  filterFactsToInstructionScope(inst, facts);
  return facts;
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryBeforeInstruction(
    Instruction *inst) const {
  auto summary = beforeSummaries.find(inst);
  if (summary != beforeSummaries.end()) {
    return ::ref_ptr<GenKillTransformer>(
        materializeLegacyWeight(summary->second));
  }
  return querySummaryAtSymbol(getProgramPointKeyBeforeInstruction(inst));
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAfterInstruction(
    Instruction *inst) const {
  auto summary = afterSummaries.find(inst);
  if (summary != afterSummaries.end()) {
    return ::ref_ptr<GenKillTransformer>(
        materializeLegacyWeight(summary->second));
  }
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
  if (auto *callInst = dyn_cast_or_null<CallBase>(inst)) {
    auto retIt = callReturnToKey.find(callInst);
    if (retIt != callReturnToKey.end()) {
      return retIt->second;
    }
  }
  auto it = instToKey.find(inst);
  return it != instToKey.end() ? it->second : WPDS_EPSILON;
}

std::unique_ptr<backend::Model> InterProceduralDataFlowEngine::buildModel(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    bool isForward) {
  using backend::ControlStateId;
  using backend::StackSymbolId;

  ::dataflow::controlflow::LLVMIntraCFG intraCfg;
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> interCfgStorage;
  if (calleeResolver) {
    interCfgStorage = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        &m, [this](Instruction *instruction) -> std::vector<Function *> {
          auto *call = dyn_cast<CallBase>(instruction);
          return call ? calleeResolver(call) : std::vector<Function *>{};
        });
  } else {
    interCfgStorage =
        std::make_unique<::dataflow::controlflow::LLVMInterCFG>(&m);
  }
  auto &interCfg = *interCfgStorage;

  auto model = std::make_unique<backend::Model>();
  const ControlStateId control = model->addControlState("q");

  modelFunctionEntry.clear();
  modelFunctionExit.clear();
  modelInstAfter.clear();
  modelInstBefore.clear();
  modelInstTransfer.clear();
  modelBasicBlock.clear();
  modelCallReturn.clear();
  localGenByInst.clear();
  localKillByInst.clear();
  functionToKey.clear();
  functionExitToKey.clear();
  instToKey.clear();
  instPrevKey.clear();
  bbToKey.clear();
  callReturnToKey.clear();
  keyToInst.clear();

  std::map<Function *, std::size_t> functionNumbers;
  std::size_t functionNumber = 0;
  for (Function &function : m) {
    if (!function.isDeclaration()) {
      functionNumbers[&function] = functionNumber++;
    }
  }

  auto functionTag = [&](Function &function) {
    std::string name = function.getName().str();
    if (name.empty()) {
      name = "anonymous";
    }
    return "f" + std::to_string(functionNumbers.at(&function)) + "_" + name;
  };

  for (Function &function : m) {
    if (function.isDeclaration()) {
      continue;
    }
    const std::string tag = functionTag(function);
    StackSymbolId entry = model->addStackSymbol("entry_" + tag);
    StackSymbolId exit = model->addStackSymbol("exit_" + tag);
    modelFunctionEntry[&function] = entry;
    modelFunctionExit[&function] = exit;
    model->addProcedureEntry(entry);
  }

  for (Function &function : m) {
    if (function.isDeclaration()) {
      continue;
    }
    std::size_t blockNumber = 0;
    for (BasicBlock &block : function) {
      const std::string blockTag =
          functionTag(function) + "_bb" + std::to_string(blockNumber++);
      StackSymbolId blockSymbol = model->addStackSymbol(blockTag);
      modelBasicBlock[&block] = blockSymbol;
      StackSymbolId previous = blockSymbol;
      std::size_t instructionNumber = 0;
      for (Instruction &instruction : block) {
        const std::string instructionTag =
            blockTag + "_i" + std::to_string(instructionNumber++);
        StackSymbolId instructionSymbol = model->addStackSymbol(instructionTag);
        modelInstTransfer[&instruction] = instructionSymbol;
        modelInstAfter[&instruction] = instructionSymbol;
        modelInstBefore[&instruction] = previous;
        if (auto *call = dyn_cast<CallBase>(&instruction)) {
          StackSymbolId returnSymbol =
              model->addStackSymbol(instructionTag + "_return");
          modelCallReturn[call] = returnSymbol;
          modelInstAfter[&instruction] = returnSymbol;
          previous = returnSymbol;
        } else {
          previous = instructionSymbol;
        }
      }
    }
  }

  auto afterSymbol = [&](Instruction *instruction) -> StackSymbolId {
    auto it = modelInstAfter.find(instruction);
    return it == modelInstAfter.end() ? backend::Epsilon : it->second;
  };
  auto popBoundary = [&](StackSymbolId symbol, const std::string &origin) {
    model->addPopRule(control, symbol, control, GenKillValue::one(), origin);
  };
  auto instructionOrigin = [](Instruction &instruction) {
    std::string text;
    raw_string_ostream stream(text);
    instruction.print(stream);
    return stream.str();
  };

  for (Function &function : m) {
    if (function.isDeclaration()) {
      continue;
    }
    const StackSymbolId functionEntry = modelFunctionEntry.at(&function);
    const StackSymbolId functionExit = modelFunctionExit.at(&function);
    BasicBlock &entryBlock = function.getEntryBlock();
    const StackSymbolId entryBlockSymbol = modelBasicBlock.at(&entryBlock);

    if (isForward) {
      model->addReplaceRule(control, functionEntry, control, entryBlockSymbol,
                            GenKillValue::one(), "function entry");
    } else {
      model->addReplaceRule(control, entryBlockSymbol, control, functionEntry,
                            GenKillValue::one(), "function entry (reverse)");
    }

    for (BasicBlock &block : function) {
      const StackSymbolId blockSymbol = modelBasicBlock.at(&block);
      if (!isForward && &block != &entryBlock) {
        for (BasicBlock *predecessor : predecessors(&block)) {
          if (predecessor == nullptr ||
              predecessor->getTerminator() == nullptr) {
            continue;
          }
          model->addReplaceRule(control, blockSymbol, control,
                                afterSymbol(predecessor->getTerminator()),
                                GenKillValue::one(), "CFG edge (reverse)");
        }
      }

      for (Instruction &instruction : block) {
        const StackSymbolId before = modelInstBefore.at(&instruction);
        const StackSymbolId transferAfter = modelInstTransfer.at(&instruction);

        ::ref_ptr<GenKillTransformer> transformer(
            createTransformer(&instruction));
        if (!transformer.get_ptr()) {
          transformer = GenKillTransformer::one();
        }
        localGenByInst[&instruction] = transformer->getGen().getFacts();
        localKillByInst[&instruction] = transformer->getKill().getFacts();
        const std::string origin = instructionOrigin(instruction);

        if (isForward) {
          model->addReplaceRule(control, before, control, transferAfter,
                                transformer->getValue(), origin);
        } else {
          model->addReplaceRule(control, transferAfter, control, before,
                                transformer->getValue(), origin);
        }

        auto *call = dyn_cast<CallBase>(&instruction);
        if (call != nullptr) {
          const StackSymbolId after = modelCallReturn.at(call);
          std::vector<Function *> callees =
              calleeResolver ? calleeResolver(call)
                             : interCfg.getCalleesOfCallAt(call);
          bool hasModeledCallee = false;
          bool hasUnmodeledCallee = callees.empty();
          std::size_t calleeNumber = 0;

          for (Function *callee : callees) {
            if (callee == nullptr || callee->isDeclaration() ||
                modelFunctionEntry.count(callee) == 0) {
              hasUnmodeledCallee = true;
              continue;
            }
            hasModeledCallee = true;
            std::map<Value *, DataFlowFacts> actualToFormal;
            std::map<Value *, DataFlowFacts> formalToActual;
            unsigned argumentIndex = 0;
            for (Argument &formal : callee->args()) {
              if (argumentIndex < call->arg_size()) {
                Value *actual = call->getArgOperand(argumentIndex);
                actualToFormal[actual].addFact(&formal);
                formalToActual[&formal].addFact(actual);
              }
              ++argumentIndex;
            }

            std::map<Value *, DataFlowFacts> returnToCall;
            std::map<Value *, DataFlowFacts> callToReturn;
            if (!call->getType()->isVoidTy()) {
              for (BasicBlock &calleeBlock : *callee) {
                auto *returnInstruction =
                    dyn_cast<ReturnInst>(calleeBlock.getTerminator());
                if (returnInstruction == nullptr ||
                    returnInstruction->getReturnValue() == nullptr) {
                  continue;
                }
                Value *returnValue = returnInstruction->getReturnValue();
                returnToCall[returnValue].addFact(call);
                callToReturn[call].addFact(returnValue);
              }
            }

            StackSymbolId pathContinuation = model->addStackSymbol(
                model->stackSymbolName(after) + "_callee" +
                std::to_string(calleeNumber++));
            if (isForward) {
              model->addPushRule(
                  control, transferAfter, control,
                  modelFunctionEntry.at(callee), pathContinuation,
                  GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                           DataFlowFacts::EmptySet(),
                                           actualToFormal),
                  origin + " (call)");
              popBoundary(modelFunctionExit.at(callee),
                          origin + " (callee return)");
              model->addReplaceRule(
                  control, pathContinuation, control, after,
                  GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                           DataFlowFacts::EmptySet(),
                                           returnToCall),
                  origin + " (return value)");
            } else {
              model->addPushRule(control, after, control,
                                 modelFunctionExit.at(callee), pathContinuation,
                                 GenKillValue::normalized(
                                     DataFlowFacts::EmptySet(),
                                     DataFlowFacts::EmptySet(), callToReturn),
                                 origin + " (reverse call)");
              popBoundary(modelFunctionEntry.at(callee),
                          origin + " (reverse callee return)");
              model->addReplaceRule(
                  control, pathContinuation, control, transferAfter,
                  GenKillValue::normalized(DataFlowFacts::EmptySet(),
                                           DataFlowFacts::EmptySet(),
                                           formalToActual),
                  origin + " (reverse arguments)");
            }
          }

          if (hasUnmodeledCallee) {
            StackSymbolId unknown = model->addStackSymbol(
                model->stackSymbolName(after) + "_unknown");
            ::ref_ptr<GenKillTransformer> summary(
                buildUnknownCallSummary(call, m, isForward));
            if (isForward) {
              model->addReplaceRule(control, transferAfter, control, unknown,
                                    summary->getValue(),
                                    origin + " (external)");
              model->addReplaceRule(control, unknown, control, after,
                                    GenKillValue::one(),
                                    origin + " (external return)");
            } else {
              model->addReplaceRule(control, after, control, unknown,
                                    summary->getValue(),
                                    origin + " (reverse external)");
              model->addReplaceRule(control, unknown, control, transferAfter,
                                    GenKillValue::one(),
                                    origin + " (reverse external return)");
            }
          }

          if (isForward && (hasModeledCallee || hasUnmodeledCallee) &&
              instruction.isTerminator()) {
            for (Instruction *returnSite :
                 interCfg.getReturnSitesOfCallAt(call)) {
              if (returnSite != nullptr &&
                  modelBasicBlock.count(returnSite->getParent()) != 0) {
                model->addReplaceRule(
                    control, after, control,
                    modelBasicBlock.at(returnSite->getParent()),
                    GenKillValue::one(), origin + " (invoke return site)");
              }
            }
          }
          continue;
        }

        if (isa<ReturnInst>(&instruction)) {
          if (isForward) {
            model->addReplaceRule(control, transferAfter, control, functionExit,
                                  GenKillValue::one(), "function return");
          } else {
            model->addReplaceRule(control, functionExit, control, transferAfter,
                                  GenKillValue::one(),
                                  "function return (reverse)");
          }
        }
      }

      if (isForward) {
        Instruction *terminator = block.getTerminator();
        if (terminator != nullptr && !isa<ReturnInst>(terminator) &&
            !isa<CallBase>(terminator)) {
          for (Instruction *successor : intraCfg.getSuccsOf(
                   terminator,
                   ::dataflow::controlflow::FlowDirection::Forward)) {
            if (successor != nullptr) {
              model->addReplaceRule(control, afterSymbol(terminator), control,
                                    modelBasicBlock.at(successor->getParent()),
                                    GenKillValue::one(), "CFG edge");
            }
          }
        }
      }
    }
  }
  return model;
}

void InterProceduralDataFlowEngine::buildWPDS(
    Module &m, WPDS<GenKillTransformer> &legacyPds,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    bool isForward) {
  static std::atomic<std::uint64_t> nextSession{0};
  const std::string prefix =
      "lotus_wpds_compat_" + std::to_string(++nextSession) + "_";
  std::unique_ptr<backend::Model> model =
      buildModel(m, createTransformer, isForward);

  std::vector<wpds_key_t> states;
  states.reserve(model->controlStateNames().size());
  for (const std::string &name : model->controlStateNames()) {
    states.push_back(new_str2key((prefix + "p_" + name).c_str()));
  }
  std::vector<wpds_key_t> symbols(model->stackSymbolNames().size(),
                                  WPDS_EPSILON);
  for (backend::StackSymbolId id = 1; id < model->stackSymbolNames().size();
       ++id) {
    symbols[id] =
        new_str2key((prefix + "g_" + model->stackSymbolName(id)).c_str());
  }

  controlState = states.front();
  functionToKey.clear();
  functionExitToKey.clear();
  instToKey.clear();
  instPrevKey.clear();
  bbToKey.clear();
  callReturnToKey.clear();
  keyToInst.clear();

  for (const auto &entry : modelFunctionEntry) {
    functionToKey[entry.first] = symbols[entry.second];
  }
  for (const auto &entry : modelFunctionExit) {
    functionExitToKey[entry.first] = symbols[entry.second];
  }
  for (const auto &entry : modelInstTransfer) {
    instToKey[entry.first] = symbols[entry.second];
    keyToInst[symbols[entry.second]] = entry.first;
  }
  for (const auto &entry : modelInstBefore) {
    instPrevKey[entry.first] = symbols[entry.second];
  }
  for (const auto &entry : modelBasicBlock) {
    bbToKey[entry.first] = symbols[entry.second];
  }
  for (const auto &entry : modelCallReturn) {
    callReturnToKey[entry.first] = symbols[entry.second];
  }

  for (const backend::Rule &rule : model->rules()) {
    GenKillTransformer *weight = materializeLegacyWeight(rule.weight);
    switch (rule.kind) {
    case backend::RuleKind::Pop:
      legacyPds.add_rule(states[rule.fromState], symbols[rule.fromStack],
                         states[rule.toState], weight);
      break;
    case backend::RuleKind::Replace:
      legacyPds.add_rule(states[rule.fromState], symbols[rule.fromStack],
                         states[rule.toState], symbols[rule.toStack1], weight);
      break;
    case backend::RuleKind::Push:
      legacyPds.add_rule(states[rule.fromState], symbols[rule.fromStack],
                         states[rule.toState], symbols[rule.toStack1],
                         symbols[rule.toStack2], weight);
      break;
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
    std::optional<std::set<Value *>> gen;
    std::optional<std::set<Value *>> kill;
  };
  std::unordered_map<wpds_key_t, KeyQueryResult> cache;

  auto querySymbol = [&](wpds_key_t sym,
                         bool wantGenKill) -> const KeyQueryResult & {
    auto it = cache.find(sym);
    if (it != cache.end() && (!wantGenKill || (it->second.gen.has_value() &&
                                               it->second.kill.has_value()))) {
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
    wpds_key_t afterKey = getProgramPointKeyAfterInstruction(inst);

    // OUT at instruction = value at the "after-inst" program-point symbol.
    result->OUT(inst) = querySymbol(afterKey, /*wantGenKill=*/true).facts;
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

void InterProceduralDataFlowEngine::extractModelResults(
    const backend::QueryResult &backendResult,
    std::unique_ptr<mono::DataFlowResult> &result) {
  auto observation = [&](std::uint32_t symbol) -> const backend::Observation * {
    auto it = backendResult.observations.find(symbol);
    return it == backendResult.observations.end() ? nullptr : &it->second;
  };

  for (const auto &entry : modelInstAfter) {
    Instruction *instruction = entry.first;
    const backend::Observation *after = observation(entry.second);
    if (after != nullptr && after->reachable) {
      result->OUT(instruction) =
          after->summary.apply(DataFlowFacts::EmptySet()).getFacts();
      afterSummaries[instruction] = after->summary;
    }
    filterFactsToInstructionScope(instruction, result->OUT(instruction));

    auto beforeSymbol = modelInstBefore.find(instruction);
    if (beforeSymbol != modelInstBefore.end()) {
      const backend::Observation *before = observation(beforeSymbol->second);
      if (before != nullptr && before->reachable) {
        result->IN(instruction) =
            before->summary.apply(DataFlowFacts::EmptySet()).getFacts();
        beforeSummaries[instruction] = before->summary;
      }
      filterFactsToInstructionScope(instruction, result->IN(instruction));
    }

    auto gen = localGenByInst.find(instruction);
    if (gen != localGenByInst.end()) {
      result->GEN(instruction) = gen->second;
      filterFactsToInstructionScope(instruction, result->GEN(instruction));
    }
    auto kill = localKillByInst.find(instruction);
    if (kill != localKillByInst.end()) {
      result->KILL(instruction) = kill->second;
      filterFactsToInstructionScope(instruction, result->KILL(instruction));
    }
  }
}

const wpds::CA<GenKillTransformer> *
InterProceduralDataFlowEngine::getLastResultAutomaton() const {
  return lastResultCA.get();
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAtSymbol(
    wpds::wpds_key_t symbol) const {
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

std::set<Value *> InterProceduralDataFlowEngine::queryFactsAtSymbol(
    wpds::wpds_key_t symbol) const {
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
    CallBase *callInst, Module &m, bool isForward) const {
  if (callInst == nullptr) {
    return GenKillTransformer::one();
  }

  std::vector<Value *> pointerObjects =
      MemoryObjectFact::pointerArgumentObjects(callInst);
  std::vector<GlobalValue *> globals = MemoryObjectFact::trackedGlobals(m);

  if (externalCallPolicy.buildSummary) {
    if (GenKillTransformer *custom = externalCallPolicy.buildSummary(
            callInst, pointerObjects, globals)) {
      return custom;
    }
  }

  DataFlowFacts killFacts = DataFlowFacts::EmptySet();
  std::set<Value *> genSet;
  std::map<Value *, DataFlowFacts> flow;
  if (!externalCallPolicy.preserveIdentity) {
    for (Value *object : pointerObjects) {
      MemoryObjectFact::addRepresentativeFact(killFacts, object);
    }
    for (GlobalValue *global : globals) {
      MemoryObjectFact::addRepresentativeFact(killFacts, global);
    }
  }

  if (!callInst->getType()->isVoidTy()) {
    const bool mayFlowFromPointers =
        externalCallPolicy.flowPointerArgumentsToReturn &&
        !pointerObjects.empty();
    const bool mayFlowFromGlobals =
        externalCallPolicy.flowGlobalsToReturn && !globals.empty();
    const bool mayGenerateReturn = mayFlowFromPointers || mayFlowFromGlobals;

    if (isForward) {
      if (mayFlowFromPointers && externalCallPolicy.preserveIdentity) {
        for (Value *object : pointerObjects) {
          MemoryObjectFact::addFlow(flow, object, callInst);
        }
      }
      if (mayFlowFromGlobals && externalCallPolicy.preserveIdentity) {
        for (GlobalValue *global : globals) {
          MemoryObjectFact::addFlow(flow, global, callInst);
        }
      }
      if (!externalCallPolicy.preserveIdentity && mayGenerateReturn) {
        genSet.insert(callInst);
      }
    } else {
      if (mayFlowFromPointers) {
        for (Value *object : pointerObjects) {
          MemoryObjectFact::addFlow(flow, callInst, object);
        }
      }
      if (mayFlowFromGlobals) {
        for (GlobalValue *global : globals) {
          MemoryObjectFact::addFlow(flow, callInst, global);
        }
      }
    }
  }

  return GenKillTransformer::makeGenKillTransformer(
      killFacts, DataFlowFacts(genSet), flow);
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
  if (!lastAcceptState.has_value()) {
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
