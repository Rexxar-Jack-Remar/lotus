/**
 * @file JoinTargetTransfer.cpp
 * @brief State transfer and summary logic for join-target analysis
 */

#include "Concurrency/JoinTarget/JoinTargetAnalysis.h"

#include <algorithm>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace mhp {

namespace {

constexpr uint8_t lifecycleBit(ThreadLifecycle lifecycle) {
  return static_cast<uint8_t>(lifecycle);
}

bool threadInstanceLess(const ThreadInstance &lhs, const ThreadInstance &rhs) {
  if (lhs.fork_site != rhs.fork_site) {
    return lhs.fork_site < rhs.fork_site;
  }
  if (lhs.epoch_class != rhs.epoch_class) {
    return lhs.epoch_class < rhs.epoch_class;
  }
  return static_cast<uint8_t>(lhs.execution_class) <
         static_cast<uint8_t>(rhs.execution_class);
}

template <typename T, typename Less>
void sortAndUnique(std::vector<T> &values, Less less) {
  std::sort(values.begin(), values.end(), less);
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

template <typename T>
void sortAndUnique(std::vector<T> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

void addLifecycle(InstanceState &state, ThreadLifecycle lifecycle) {
  state.lifecycle_mask |= lifecycleBit(lifecycle);
}

void removeLifecycle(InstanceState &state, ThreadLifecycle lifecycle) {
  state.lifecycle_mask &= ~lifecycleBit(lifecycle);
}

bool transitionInstance(InstanceState &state, ThreadLifecycle terminal,
                        bool keepJoinable = false) {
  const InstanceState before = state;
  if (state.mayBeJoinable() && !keepJoinable) {
    removeLifecycle(state, ThreadLifecycle::Joinable);
  }
  addLifecycle(state, terminal);
  return !(before == state);
}

bool mergeInstanceState(InstanceState &dst, const InstanceState &src) {
  const InstanceState before = dst;
  dst.lifecycle_mask |= src.lifecycle_mask;
  dst.creation_may_fail |= src.creation_may_fail;
  dst.join_may_fail |= src.join_may_fail;
  return !(before == dst);
}

bool transitionHandleState(HandleState &state, ThreadLifecycle terminal,
                           bool keepJoinable = false) {
  bool changed = false;
  for (auto &entry : state.instances) {
    changed |= transitionInstance(entry.second, terminal, keepJoinable);
  }
  if (terminal == ThreadLifecycle::Joined ||
      terminal == ThreadLifecycle::Detached ||
      terminal == ThreadLifecycle::Overwritten) {
    if (!state.preserved_arg_inputs.empty()) {
      state.preserved_arg_inputs.clear();
      changed = true;
    }
  }
  return changed;
}

bool transitionLocationFamily(JoinTargetStateMap &state,
                              const HandleLocation &location,
                              ThreadLifecycle terminal,
                              bool keepJoinable = false) {
  bool changed = false;
  auto transitionEntry = [&](HandleState &handleState) {
    changed |= transitionHandleState(handleState, terminal, keepJoinable);
  };

  if (location.is_base_wildcard) {
    for (auto &entry : state) {
      if (entry.first.base == location.base) {
        transitionEntry(entry.second);
      }
    }
    return changed;
  }

  auto it = state.find(location);
  if (it != state.end()) {
    transitionEntry(it->second);
  }

  HandleLocation wildcard = location;
  wildcard.offsets.clear();
  wildcard.is_base_wildcard = true;
  auto wildcardIt = state.find(wildcard);
  if (wildcardIt != state.end()) {
    transitionEntry(wildcardIt->second);
  }

  return changed;
}

std::vector<ThreadInstance> collectInstances(const HandleState &state,
                                             bool feasibleOnly) {
  std::vector<ThreadInstance> instances;
  for (const auto &entry : state.instances) {
    if (feasibleOnly && !entry.second.mayBeJoinable()) {
      continue;
    }
    instances.push_back(entry.first);
  }
  sortAndUnique(instances, threadInstanceLess);
  return instances;
}

std::vector<const Instruction *>
collectForks(const std::vector<ThreadInstance> &instances) {
  std::vector<const Instruction *> forks;
  forks.reserve(instances.size());
  for (const ThreadInstance &instance : instances) {
    if (instance.fork_site) {
      forks.push_back(instance.fork_site);
    }
  }
  sortAndUnique(forks);
  return forks;
}

std::vector<const Value *> collectRoots(const HandleState &state) {
  std::vector<const Value *> roots(state.provenance_roots.begin(),
                                   state.provenance_roots.end());
  sortAndUnique(roots);
  return roots;
}

bool sameInstances(const std::vector<ThreadInstance> &lhs,
                   const std::vector<ThreadInstance> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (!(lhs[i] == rhs[i])) {
      return false;
    }
  }
  return true;
}

} // namespace

std::size_t ThreadInstanceHash::operator()(
    const ThreadInstance &instance) const {
  std::size_t seed = std::hash<const Instruction *>{}(instance.fork_site);
  seed ^= std::hash<uint8_t>{}(instance.epoch_class) + 0x9e3779b9 +
          (seed << 6) + (seed >> 2);
  seed ^= std::hash<uint8_t>{}(static_cast<uint8_t>(instance.execution_class)) +
          0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

void JoinTargetAnalysis::buildFunctionSummaries() {
  bool changed = true;
  while (changed) {
    changed = false;

    for (const Function &func : m_module) {
      if (func.isDeclaration()) {
        continue;
      }

      std::unordered_map<const BasicBlock *, StateMap> inStates;
      std::unordered_map<const BasicBlock *, StateMap> outStates;

      bool localChanged = true;
      while (localChanged) {
        localChanged = false;
        for (const BasicBlock &block : func) {
          StateMap inState;
          if (&block == &func.getEntryBlock()) {
            inState = getEntryStateForFunction(func);
          } else {
            for (const BasicBlock *pred : predecessors(&block)) {
              auto predIt = outStates.find(pred);
              if (predIt != outStates.end()) {
                mergeStateInto(inState, predIt->second);
              }
            }
          }

          auto inIt = inStates.find(&block);
          if (inIt == inStates.end() || !(inIt->second == inState)) {
            inStates[&block] = inState;
            localChanged = true;
          }

          StateMap outState = inState;
          for (const Instruction &inst : block) {
            transferInstruction(inst, outState);
          }

          auto outIt = outStates.find(&block);
          if (outIt == outStates.end() || !(outIt->second == outState)) {
            outStates[&block] = outState;
            localChanged = true;
          }
        }
      }

      FunctionSummary summary;
      for (const BasicBlock &block : func) {
        const Instruction *term = block.getTerminator();
        if (!isa<ReturnInst>(term) && !isa<ResumeInst>(term)) {
          continue;
        }

        auto outIt = outStates.find(&block);
        if (outIt == outStates.end()) {
          continue;
        }

        for (const auto &entry : outIt->second) {
          const auto *arg = dyn_cast<Argument>(entry.first.base);
          if (!arg || arg->getParent() != &func) {
            continue;
          }
          SummaryLocation summaryLocation;
          summaryLocation.arg_no = arg->getArgNo();
          summaryLocation.offsets = entry.first.offsets;
          summaryLocation.is_base_wildcard = entry.first.is_base_wildcard;
          mergeHandleState(summary.location_exit_states[summaryLocation],
                           entry.second);
        }
      }

      auto summaryIt = m_functionSummaries.find(&func);
      if (summaryIt == m_functionSummaries.end() ||
          summaryIt->second.location_exit_states !=
              summary.location_exit_states) {
        m_functionSummaries[&func] = std::move(summary);
        changed = true;
      }
    }
  }
}

JoinTargetAnalysis::StateMap
JoinTargetAnalysis::getEntryStateForFunction(const Function &func) const {
  StateMap state;
  for (const Argument &arg : func.args()) {
    if (!arg.getType()->isPointerTy()) {
      continue;
    }
    HandleState argState;
    argState.preserved_arg_inputs.insert(arg.getArgNo());
    argState.provenance_roots.insert(&arg);
    state[HandleLocation{&arg, {}, false}] = std::move(argState);
  }
  return state;
}

JoinTargetAnalysis::StateMap
JoinTargetAnalysis::mergePredecessorStates(const BasicBlock *block) const {
  StateMap merged;
  if (!block) {
    return merged;
  }

  if (block == &block->getParent()->getEntryBlock()) {
    return getEntryStateForFunction(*block->getParent());
  }

  for (const BasicBlock *pred : predecessors(block)) {
    auto it = m_blockOutStates.find(pred);
    if (it != m_blockOutStates.end()) {
      mergeStateInto(merged, it->second);
    }
  }
  return merged;
}

void JoinTargetAnalysis::analyzeFunction(const Function &func) {
  m_blockInStates.clear();
  m_blockOutStates.clear();

  bool changed = true;
  while (changed) {
    changed = false;
    for (const BasicBlock &block : func) {
      StateMap inState = mergePredecessorStates(&block);
      auto inIt = m_blockInStates.find(&block);
      if (inIt == m_blockInStates.end() || !(inIt->second == inState)) {
        m_blockInStates[&block] = inState;
        changed = true;
      }

      StateMap outState = inState;
      for (const Instruction &inst : block) {
        if (m_threadAPI->isTDJoin(&inst)) {
          recordJoinState(&inst, outState);
        }
        transferInstruction(inst, outState);
      }

      auto outIt = m_blockOutStates.find(&block);
      if (outIt == m_blockOutStates.end() || !(outIt->second == outState)) {
        m_blockOutStates[&block] = outState;
        changed = true;
      }
    }
  }
}

bool JoinTargetAnalysis::transferInstruction(const Instruction &inst,
                                             StateMap &state) const {
  if (const auto *store = dyn_cast<StoreInst>(&inst)) {
    HandleState storedState = getStateForValue(store->getValueOperand(), state);
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(store->getPointerOperand())) {
      HandleState newState = storedState;
      if (location.is_base_wildcard) {
        newState.path_sensitivity_lost = true;
        changed |= killLocationFamily(state, location);
      }
      changed |= overwriteLocation(state, location, newState);
    }
    return changed;
  }

  if (isa<CallBase>(&inst)) {
    return applyCallEffect(inst, state);
  }

  return false;
}

ThreadInstance
JoinTargetAnalysis::makeThreadInstance(const Instruction &forkInst) const {
  ThreadInstance instance;
  instance.fork_site = &forkInst;
  const bool repeated =
      m_threadMultiplicity &&
      m_threadMultiplicity->instructionMayExecuteMultipleTimes(&forkInst);
  instance.execution_class = repeated ? ThreadExecutionClass::RepeatedExecution
                                      : ThreadExecutionClass::SingleExecution;
  instance.epoch_class = repeated ? 1 : 0;
  return instance;
}

bool JoinTargetAnalysis::applyCallEffect(const Instruction &inst,
                                         StateMap &state) const {
  const auto *call = dyn_cast<CallBase>(&inst);
  if (!call) {
    return false;
  }

  const Function *callee = m_threadAPI->getCallee(&inst);
  if (m_threadAPI->isTDFork(&inst)) {
    HandleState forkState;
    InstanceState instanceState;
    addLifecycle(instanceState, ThreadLifecycle::Joinable);
    forkState.instances.emplace(makeThreadInstance(inst), instanceState);
    forkState.provenance_roots.insert(
        traceThreadHandleRoot(m_threadAPI->getForkedThread(&inst), &m_module));

    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(m_threadAPI->getForkedThread(&inst))) {
      if (location.base) {
        forkState.provenance_roots.insert(location.base);
      }
      if (location.is_base_wildcard) {
        forkState.path_sensitivity_lost = true;
        changed |= killLocationFamily(state, location);
      }
      changed |= overwriteLocation(state, location, forkState);
    }
    return changed;
  }

  if (m_threadAPI->isTDJoin(&inst)) {
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(m_threadAPI->getJoinedThread(&inst))) {
      changed |= transitionLocationFamily(state, location,
                                          ThreadLifecycle::Joined, false);
    }
    return changed;
  }

  if (callee && m_threadAPI->getType(callee) == ThreadAPI::TD_DETACH) {
    if (call->arg_size() < 1) {
      return false;
    }
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(call->getArgOperand(0))) {
      changed |= transitionLocationFamily(state, location,
                                          ThreadLifecycle::Detached, false);
    }
    return changed;
  }

  if (callee && !callee->isDeclaration()) {
    return applyDirectCallSummary(*call, *callee, state);
  }

  bool changed = false;
  for (unsigned i = 0; i < call->arg_size(); ++i) {
    const Value *arg = call->getArgOperand(i);
    if (!arg || !arg->getType()->isPointerTy()) {
      continue;
    }
    for (const HandleLocation &location : resolveWriteLocations(arg)) {
      auto it = state.find(location);
      if (it == state.end()) {
        continue;
      }
      const HandleState before = it->second;
      it->second.has_unknown_live_fork = true;
      transitionHandleState(it->second, ThreadLifecycle::Escaped, true);
      changed |= !(before == it->second);
    }
  }
  return changed;
}

bool JoinTargetAnalysis::applyDirectCallSummary(const CallBase &call,
                                                const Function &callee,
                                                StateMap &state) const {
  auto summaryIt = m_functionSummaries.find(&callee);
  if (summaryIt == m_functionSummaries.end()) {
    return false;
  }

  bool changed = false;
  for (const auto &entry : summaryIt->second.location_exit_states) {
    unsigned argNo = entry.first.arg_no;
    if (argNo >= call.arg_size()) {
      continue;
    }

    HandleState resolved;
    if (entry.second.instances.empty()) {
      for (unsigned preservedArg : entry.second.preserved_arg_inputs) {
        if (preservedArg >= call.arg_size()) {
          continue;
        }
        mergeHandleState(
            resolved, getStateForValue(call.getArgOperand(preservedArg), state));
      }
    }

    mergeHandleState(resolved, entry.second);

    for (const HandleLocation &location :
         resolveWriteLocations(call.getArgOperand(argNo))) {
      HandleLocation mappedLocation = location;
      if (mappedLocation.is_base_wildcard || entry.first.is_base_wildcard) {
        mappedLocation.offsets.clear();
        mappedLocation.is_base_wildcard = true;
        resolved.path_sensitivity_lost = true;
      } else {
        mappedLocation.offsets.insert(mappedLocation.offsets.end(),
                                      entry.first.offsets.begin(),
                                      entry.first.offsets.end());
      }

      if (mappedLocation.is_base_wildcard) {
        changed |= killLocationFamily(state, mappedLocation);
      }
      changed |= overwriteLocation(state, mappedLocation, resolved);
    }
  }
  return changed;
}

JoinResolution
JoinTargetAnalysis::buildResolutionFromState(const HandleState &state) const {
  JoinResolution resolution;
  resolution.possible_instances = collectInstances(state, false);
  resolution.feasible_instances = collectInstances(state, true);
  resolution.possible_forks = collectForks(resolution.possible_instances);
  resolution.feasible_forks = collectForks(resolution.feasible_instances);
  resolution.related_handle_roots = collectRoots(state);
  resolution.has_unknown_live_instance = state.has_unknown_live_fork;
  return resolution;
}

JoinResolution JoinTargetAnalysis::buildJoinResolution(const Instruction *joinInst,
                                                       const StateMap &state) const {
  JoinResolution resolution;
  if (!joinInst) {
    return resolution;
  }

  const Value *joinedValue = m_threadAPI->getJoinedThread(joinInst);
  const Value *stripped = joinedValue ? joinedValue->stripPointerCasts() : nullptr;

  auto appendAlternative = [&](JoinPathAlternative alternative) {
    if (alternative.incoming_block || !alternative.feasible_instances.empty() ||
        !alternative.possible_instances.empty() ||
        alternative.has_unknown_live_instance ||
        !alternative.related_handle_roots.empty()) {
      resolution.path_alternatives.push_back(std::move(alternative));
    }
  };

  if (const auto *phi = dyn_cast_or_null<PHINode>(stripped)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      const BasicBlock *incomingBlock = phi->getIncomingBlock(i);
      const StateMap *incomingState = &state;
      auto outIt = m_blockOutStates.find(incomingBlock);
      if (outIt != m_blockOutStates.end()) {
        incomingState = &outIt->second;
      }
      JoinResolution altResolution =
          buildResolutionFromState(getStateForValue(phi->getIncomingValue(i),
                                                    *incomingState));
      JoinPathAlternative alternative;
      alternative.incoming_block = incomingBlock;
      alternative.possible_instances = std::move(altResolution.possible_instances);
      alternative.feasible_instances = std::move(altResolution.feasible_instances);
      alternative.possible_forks = std::move(altResolution.possible_forks);
      alternative.feasible_forks = std::move(altResolution.feasible_forks);
      alternative.related_handle_roots =
          std::move(altResolution.related_handle_roots);
      alternative.has_unknown_live_instance =
          altResolution.has_unknown_live_instance;
      appendAlternative(std::move(alternative));
    }
  } else if (const auto *select = dyn_cast_or_null<SelectInst>(stripped)) {
    for (const Value *arm : {select->getTrueValue(), select->getFalseValue()}) {
      JoinResolution altResolution =
          buildResolutionFromState(getStateForValue(arm, state));
      JoinPathAlternative alternative;
      alternative.possible_instances = std::move(altResolution.possible_instances);
      alternative.feasible_instances = std::move(altResolution.feasible_instances);
      alternative.possible_forks = std::move(altResolution.possible_forks);
      alternative.feasible_forks = std::move(altResolution.feasible_forks);
      alternative.related_handle_roots =
          std::move(altResolution.related_handle_roots);
      alternative.has_unknown_live_instance =
          altResolution.has_unknown_live_instance;
      appendAlternative(std::move(alternative));
    }
  }

  if (resolution.path_alternatives.empty()) {
    resolution = buildResolutionFromState(getStateForValue(joinedValue, state));
  } else {
    resolution.is_path_sensitive = resolution.path_alternatives.size() > 1;
    for (const JoinPathAlternative &alternative : resolution.path_alternatives) {
      resolution.possible_instances.insert(resolution.possible_instances.end(),
                                           alternative.possible_instances.begin(),
                                           alternative.possible_instances.end());
      resolution.feasible_instances.insert(resolution.feasible_instances.end(),
                                           alternative.feasible_instances.begin(),
                                           alternative.feasible_instances.end());
      resolution.possible_forks.insert(resolution.possible_forks.end(),
                                       alternative.possible_forks.begin(),
                                       alternative.possible_forks.end());
      resolution.feasible_forks.insert(resolution.feasible_forks.end(),
                                       alternative.feasible_forks.begin(),
                                       alternative.feasible_forks.end());
      resolution.related_handle_roots.insert(
          resolution.related_handle_roots.end(),
          alternative.related_handle_roots.begin(),
          alternative.related_handle_roots.end());
      resolution.has_unknown_live_instance |=
          alternative.has_unknown_live_instance;
    }

    sortAndUnique(resolution.possible_instances, threadInstanceLess);
    sortAndUnique(resolution.feasible_instances, threadInstanceLess);
    sortAndUnique(resolution.possible_forks);
    sortAndUnique(resolution.feasible_forks);
    sortAndUnique(resolution.related_handle_roots);

    const JoinPathAlternative &first = resolution.path_alternatives.front();
    for (size_t i = 1; i < resolution.path_alternatives.size(); ++i) {
      const JoinPathAlternative &alternative = resolution.path_alternatives[i];
      if (!sameInstances(first.feasible_instances, alternative.feasible_instances) ||
          first.has_unknown_live_instance !=
              alternative.has_unknown_live_instance) {
        addAmbiguityReason(resolution,
                           JoinAmbiguityReason::PathMergedAlternatives);
        break;
      }
    }
  }

  for (const HandleLocation &location : resolveReadLocations(joinedValue)) {
    if (location.is_base_wildcard) {
      addAmbiguityReason(resolution, JoinAmbiguityReason::WildcardLocation);
      break;
    }
  }

  return resolution;
}

void JoinTargetAnalysis::finalizeJoinResolution(
    const Instruction *joinInst, JoinResolution &resolution) const {
  if (!joinInst) {
    return;
  }

  if (resolution.feasible_instances.empty()) {
    addAmbiguityReason(resolution, JoinAmbiguityReason::NoFeasibleInstance);
  }
  if (resolution.feasible_instances.size() > 1) {
    addAmbiguityReason(resolution,
                       JoinAmbiguityReason::MultipleFeasibleInstances);
  }
  if (resolution.has_unknown_live_instance) {
    addAmbiguityReason(resolution, JoinAmbiguityReason::UnknownExternalEffect);
  }

  for (const ThreadInstance &instance : resolution.feasible_instances) {
    if (instance.execution_class == ThreadExecutionClass::RepeatedExecution) {
      addAmbiguityReason(resolution, JoinAmbiguityReason::RepeatedForkSite);
      break;
    }
  }

  resolution.unambiguous = resolution.feasible_instances.size() == 1 &&
                           !resolution.has_unknown_live_instance;
  if (resolution.unambiguous) {
    const ThreadInstance &instance = resolution.feasible_instances.front();
    if (instance.execution_class == ThreadExecutionClass::RepeatedExecution) {
      resolution.unambiguous = false;
    }
  }
}

bool JoinTargetAnalysis::addAmbiguityReason(JoinResolution &resolution,
                                            JoinAmbiguityReason reason) const {
  if (reason == JoinAmbiguityReason::None) {
    return false;
  }
  if (std::find(resolution.ambiguity_reasons.begin(),
                resolution.ambiguity_reasons.end(),
                reason) != resolution.ambiguity_reasons.end()) {
    return false;
  }
  resolution.ambiguity_reasons.push_back(reason);
  return true;
}

void JoinTargetAnalysis::recordJoinState(const Instruction *join_inst,
                                         const StateMap &state) {
  if (!join_inst) {
    return;
  }

  m_joinResolutions[join_inst] = buildJoinResolution(join_inst, state);
}

bool JoinTargetAnalysis::overwriteLocation(StateMap &state,
                                           const HandleLocation &location,
                                           const HandleState &new_state) const {
  if (!location.base) {
    return false;
  }

  HandleState combined = new_state;
  auto existingIt = state.find(location);
  if (existingIt != state.end()) {
    HandleState preserved = existingIt->second;
    for (auto &entry : preserved.instances) {
      auto newIt = new_state.instances.find(entry.first);
      if (newIt == new_state.instances.end() || !newIt->second.mayBeJoinable()) {
        if (entry.second.mayBeJoinable()) {
          removeLifecycle(entry.second, ThreadLifecycle::Joinable);
          addLifecycle(entry.second, ThreadLifecycle::Overwritten);
        }
      }
    }
    mergeHandleState(combined, preserved);
  }

  auto stateIt = state.find(location);
  if (stateIt != state.end() && stateIt->second == combined) {
    return false;
  }
  state[location] = std::move(combined);
  return true;
}

bool JoinTargetAnalysis::killLocationFamily(StateMap &state,
                                            const HandleLocation &location) const {
  if (!location.base) {
    return false;
  }

  bool changed = false;
  std::vector<HandleLocation> toErase;
  for (const auto &entry : state) {
    if (entry.first.base != location.base || entry.first.is_base_wildcard) {
      continue;
    }
    toErase.push_back(entry.first);
  }
  for (const HandleLocation &child : toErase) {
    changed = true;
    state.erase(child);
  }
  return changed;
}

HandleState JoinTargetAnalysis::getStateForValue(const Value *value,
                                                 const StateMap &state) const {
  HandleState result;
  if (!value) {
    return result;
  }

  auto locations = resolveReadLocations(value);
  if (!locations.empty()) {
    bool foundAny = false;
    for (const HandleLocation &location : locations) {
      if (location.is_base_wildcard) {
        result.path_sensitivity_lost = true;
      }

      auto it = state.find(location);
      if (it != state.end()) {
        mergeHandleState(result, it->second);
        foundAny = true;
      }

      HandleLocation wildcard = location;
      wildcard.offsets.clear();
      wildcard.is_base_wildcard = true;
      if (!(wildcard == location)) {
        auto wildcardIt = state.find(wildcard);
        if (wildcardIt != state.end()) {
          mergeHandleState(result, wildcardIt->second);
          foundAny = true;
        }
      }

      if (location.is_base_wildcard) {
        for (const auto &entry : state) {
          if (entry.first.base == location.base && !entry.first.is_base_wildcard) {
            mergeHandleState(result, entry.second);
            foundAny = true;
          }
        }
      }
    }
    if (foundAny) {
      return result;
    }
  }

  const Value *stripped = value->stripPointerCasts();
  if (isa<ConstantPointerNull>(stripped)) {
    return result;
  }

  if (const auto *arg = dyn_cast<Argument>(stripped)) {
    auto it = state.find(HandleLocation{arg, {}, false});
    if (it != state.end()) {
      return it->second;
    }
  }

  if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped) ||
      isa<Argument>(stripped)) {
    result.provenance_roots.insert(stripped);
    return result;
  }

  if (value->getType()->isPointerTy()) {
    result.has_unknown_live_fork = true;
  }
  if (const Value *root = traceThreadHandleRoot(value, &m_module)) {
    result.provenance_roots.insert(root);
  }
  return result;
}

bool JoinTargetAnalysis::mergeStateInto(StateMap &dst, const StateMap &src) const {
  bool changed = false;
  for (const auto &entry : src) {
    changed |= mergeHandleState(dst[entry.first], entry.second);
  }
  return changed;
}

bool JoinTargetAnalysis::mergeHandleState(HandleState &dst,
                                          const HandleState &src) const {
  const HandleState before = dst;
  for (const auto &entry : src.instances) {
    mergeInstanceState(dst.instances[entry.first], entry.second);
  }
  dst.preserved_arg_inputs.insert(src.preserved_arg_inputs.begin(),
                                  src.preserved_arg_inputs.end());
  dst.provenance_roots.insert(src.provenance_roots.begin(),
                              src.provenance_roots.end());
  dst.has_unknown_live_fork |= src.has_unknown_live_fork;
  dst.path_sensitivity_lost |= src.path_sensitivity_lost;
  return !(before == dst);
}

} // namespace mhp
