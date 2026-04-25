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

template <typename T>
std::vector<T> toSortedVector(const std::unordered_set<T> &values) {
  std::vector<T> result(values.begin(), values.end());
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace

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

bool JoinTargetAnalysis::applyCallEffect(const Instruction &inst,
                                         StateMap &state) const {
  const auto *call = dyn_cast<CallBase>(&inst);
  if (!call) {
    return false;
  }

  const Function *callee = m_threadAPI->getCallee(&inst);
  if (m_threadAPI->isTDFork(&inst)) {
    HandleState forkState;
    forkState.live_forks.insert(&inst);
    forkState.historical_forks.insert(&inst);
    bool changed = false;
    for (const HandleLocation &location :
         resolveWriteLocations(m_threadAPI->getForkedThread(&inst))) {
      if (location.is_base_wildcard) {
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
      if (location.is_base_wildcard) {
        changed |= killLocationFamily(state, location);
      }
      changed |= overwriteLocation(state, location, HandleState{});
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
      if (location.is_base_wildcard) {
        changed |= killLocationFamily(state, location);
      }
      changed |= overwriteLocation(state, location, HandleState{});
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
      HandleState unknownState = state[location];
      unknownState.has_unknown_live_fork = true;
      if (location.is_base_wildcard) {
        changed |= killLocationFamily(state, location);
      }
      changed |= overwriteLocation(state, location, unknownState);
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
    for (unsigned preservedArg : entry.second.preserved_arg_inputs) {
      if (preservedArg >= call.arg_size()) {
        continue;
      }
      mergeHandleState(resolved,
                       getStateForValue(call.getArgOperand(preservedArg), state));
    }
    resolved.live_forks.insert(entry.second.live_forks.begin(),
                               entry.second.live_forks.end());
    resolved.historical_forks.insert(entry.second.historical_forks.begin(),
                                     entry.second.historical_forks.end());
    resolved.historical_forks.insert(resolved.live_forks.begin(),
                                     resolved.live_forks.end());
    resolved.has_unknown_live_fork |= entry.second.has_unknown_live_fork;

    for (const HandleLocation &location :
         resolveWriteLocations(call.getArgOperand(argNo))) {
      HandleLocation mappedLocation = location;
      if (mappedLocation.is_base_wildcard || entry.first.is_base_wildcard) {
        mappedLocation.offsets.clear();
        mappedLocation.is_base_wildcard = true;
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

void JoinTargetAnalysis::recordJoinState(const Instruction *join_inst,
                                         const StateMap &state) {
  if (!join_inst) {
    return;
  }

  HandleState joinState =
      getStateForValue(m_threadAPI->getJoinedThread(join_inst), state);
  m_joinToForks[join_inst] = toSortedVector(joinState.historical_forks);
  m_joinToFeasibleForks[join_inst] = toSortedVector(joinState.live_forks);
  m_joinHasUnknownLiveFork[join_inst] = joinState.has_unknown_live_fork;
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
    combined.historical_forks.insert(existingIt->second.historical_forks.begin(),
                                     existingIt->second.historical_forks.end());
  }
  combined.historical_forks.insert(combined.live_forks.begin(),
                                   combined.live_forks.end());

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

  if (value->getType()->isPointerTy()) {
    result.has_unknown_live_fork = true;
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
  dst.live_forks.insert(src.live_forks.begin(), src.live_forks.end());
  dst.historical_forks.insert(src.historical_forks.begin(),
                              src.historical_forks.end());
  dst.historical_forks.insert(dst.live_forks.begin(), dst.live_forks.end());
  dst.preserved_arg_inputs.insert(src.preserved_arg_inputs.begin(),
                                  src.preserved_arg_inputs.end());
  dst.has_unknown_live_fork |= src.has_unknown_live_fork;
  return !(before == dst);
}

} // namespace mhp
