/**
 * @file OpenMPTaskGraph.cpp
 * @brief Implementation of OpenMP Task Dependency Graph
 */

#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"

#include "Analysis/Concurrency/OpenMP/OpenMPModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <deque>

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace OpenMP;

namespace {

DependType decodeDependType(uint64_t flags) {
  if ((flags & 0x4ULL) != 0) {
    return DependType::MUTEXINOUTSET;
  }
  switch (flags & 0x3ULL) {
  case 0x1ULL:
    return DependType::IN;
  case 0x2ULL:
    return DependType::OUT;
  default:
    return DependType::INOUT;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

std::pair<DependencySourceKind, DependencyProof>
classifyDependencyAddressEvidence(const Value *value) {
  if (!value) {
    return {DependencySourceKind::RegionSummary, DependencyProof::Unknown};
  }

  value = value->stripPointerCasts();
  if (isa<GlobalValue>(value) || isa<AllocaInst>(value)) {
    return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
  }

  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->isCast() || ce->getOpcode() == Instruction::GetElementPtr) {
      const Value *base =
          getUnderlyingObject(ce->getOperand(0)->stripPointerCasts());
      if (isa<GlobalValue>(base) || isa<AllocaInst>(base)) {
        return {DependencySourceKind::DirectAddress, DependencyProof::Definite};
      }
    }
  }

  if (const auto *arg = dyn_cast<Argument>(value)) {
    return {DependencySourceKind::Iterator, arg->getType()->isPointerTy()
                                                ? DependencyProof::Possible
                                                : DependencyProof::Unknown};
  }

  return {DependencySourceKind::RegionSummary, DependencyProof::Possible};
}

const Value *canonicalizeDependencyAddress(const Value *value,
                                           const DataLayout &DL,
                                           int64_t &offset,
                                           bool &has_precise_offset) {
  offset = 0;
  has_precise_offset = false;
  if (!value) {
    return nullptr;
  }

  value = stripValue(value);
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    value = load->getPointerOperand()->stripPointerCasts();
  }

  if (const Value *base_with_offset =
          GetPointerBaseWithConstantOffset(value, offset, DL)) {
    const Value *canonical_base = stripValue(base_with_offset);
    // For variable-index GEPs, LLVM can return the original expression as the
    // "base with offset=0". Treat that as imprecise and fall back to the
    // underlying object to avoid fabricating precise non-aliasing.
    if (!(isa<GEPOperator>(value) && canonical_base == stripValue(value))) {
      has_precise_offset = true;
      return canonical_base;
    }
  }

  if (const Value *base = getUnderlyingObject(value)) {
    return stripValue(base);
  }
  return value;
}

bool decodeConstantDependency(const Constant *elt, Dependency &dep) {
  if (!elt) {
    return false;
  }
  const auto *cs = dyn_cast<ConstantStruct>(elt);
  if (!cs || cs->getNumOperands() < 3) {
    return false;
  }

  dep.address = stripValue(cs->getOperand(0));
  dep.size = 0;
  dep.type = DependType::INOUT;

  if (const auto *len = dyn_cast<ConstantInt>(cs->getOperand(1))) {
    dep.size = len->getZExtValue();
  }
  if (const auto *flags = dyn_cast<ConstantInt>(cs->getOperand(2))) {
    dep.type = decodeDependType(flags->getZExtValue());
  }
  return dep.address != nullptr;
}

bool isBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

bool mustHappenBefore(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs) {
    return false;
  }
  if (lhs == rhs) {
    return false;
  }
  if (lhs->getFunction() != rhs->getFunction()) {
    return false;
  }
  if (lhs->getParent() == rhs->getParent()) {
    return isBeforeInBlock(lhs, rhs);
  }

  const Function *func = lhs->getFunction();
  if (!func || func->isDeclaration()) {
    return false;
  }

  DominatorTree DT(*const_cast<Function *>(func));
  if (!DT.dominates(lhs, rhs)) {
    return false;
  }

  LoopInfo LI(DT);
  return !isPotentiallyReachable(rhs, lhs, nullptr, &DT, &LI);
}

std::pair<const Task *, const Task *> normalizeTaskPair(const Task *lhs,
                                                        const Task *rhs) {
  return lhs < rhs ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
}

const Instruction *taskOrderingSite(const Task *task) {
  if (!task) {
    return nullptr;
  }
  return task->generating_context ? task->generating_context
                                  : task->task_create;
}

} // namespace

OpenMPTaskGraph::OpenMPTaskGraph(Module &module) : m_module(module) {}

void OpenMPTaskGraph::analyze() {
  errs() << "Starting OpenMP Task Dependency Analysis...\n";
  m_tasks.clear();
  m_inst_to_task.clear();
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();
  m_relations.clear();
  m_next_scheduling_context_id = 1;
  m_deferred_wait_deps_count = 0;
  m_deferred_imprecise_conflict_count = 0;
  m_deferred_reason_counts.clear();
  m_summary = AnalysisSummary{};
  identifyTasks();
  buildDependencyEdges();

  m_summary.task_count = m_tasks.size();
  m_summary.wait_boundary_count = m_wait_boundary_infos.size();
  m_summary.partial_wait_boundary_count = 0;
  for (const WaitBoundaryInfo &info : m_wait_boundary_infos) {
    if (info.is_partial_wait) {
      ++m_summary.partial_wait_boundary_count;
    }
  }

  errs() << "Found " << m_tasks.size() << " OpenMP tasks with dependencies\n";
  if (m_deferred_wait_deps_count) {
    errs() << "Deferred " << m_deferred_wait_deps_count
           << " OpenMP wait_deps boundaries (partial synchronization)\n";
  }
  if (m_deferred_imprecise_conflict_count) {
    errs() << "Deferred " << m_deferred_imprecise_conflict_count
           << " imprecise OpenMP depend conflicts (no definite HB edge)\n";
  }
}

void OpenMPTaskGraph::identifyTasks() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  std::set<const Function *> directly_called;
  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    for (const BasicBlock &bb : func) {
      for (const Instruction &inst : bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call) {
          continue;
        }
        const Function *callee = call->getCalledFunction();
        if (!callee) {
          if (const Value *called = call->getCalledOperand()) {
            callee = dyn_cast<Function>(called->stripPointerCasts());
          }
        }
        if (callee && !callee->isDeclaration()) {
          directly_called.insert(callee);
        }
        if (api->isForkLike(call)) {
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            if (!fork_target->isDeclaration()) {
              directly_called.insert(fork_target);
            }
          }
        }
      }
    }
  }

  std::vector<const Function *> roots;
  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    if (!directly_called.count(&func)) {
      roots.push_back(&func);
    }
  }
  if (roots.empty()) {
    for (const Function &func : m_module) {
      if (!func.isDeclaration()) {
        roots.push_back(&func);
      }
    }
  }

  for (const Function *root : roots) {
    TraversalState state;
    state.scheduling_context_id = m_next_scheduling_context_id++;
    state.phase_stack.push_back(0);
    std::set<const Function *> call_stack;
    scanSchedulingContext(root, state, call_stack);
  }
}

void OpenMPTaskGraph::scanSchedulingContext(
    const Function *func, TraversalState &state,
    std::set<const Function *> &call_stack) {
  if (!func || func->isDeclaration() || !call_stack.insert(func).second) {
    return;
  }

  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto currentPhaseToken = [&state]() -> size_t {
    return state.phase_stack.empty() ? 0 : state.phase_stack.back();
  };
  auto currentRegionId = [&state]() -> size_t {
    return state.region_stack.empty() ? 0 : state.region_stack.back().id;
  };
  auto advanceCurrentPhase = [&state]() {
    if (state.phase_stack.empty()) {
      state.phase_stack.push_back(state.next_phase_token++);
    } else {
      state.phase_stack.back() = state.next_phase_token++;
    }
  };
  auto pushRegion = [&state](WaitBoundaryInfo::Kind kind) {
    TraversalState::RegionFrame frame;
    frame.id = state.next_region_id++;
    frame.kind = kind;
    state.region_stack.push_back(frame);
  };
  auto popRegion = [&state](WaitBoundaryInfo::Kind kind) {
    if (state.region_stack.empty()) {
      return size_t{0};
    }
    if (state.region_stack.back().kind == kind) {
      size_t region_id = state.region_stack.back().id;
      state.region_stack.pop_back();
      return region_id;
    }
    for (auto it = state.region_stack.rbegin(); it != state.region_stack.rend();
         ++it) {
      if (it->kind == kind) {
        return it->id;
      }
    }
    return size_t{0};
  };
  auto recordBoundary = [&](const CallBase *call, WaitBoundaryInfo::Kind kind,
                            bool partial_wait = false,
                            bool taskgroup_end = false) {
    WaitBoundaryInfo info;
    info.inst = call;
    info.scheduling_context_id = state.scheduling_context_id;
    info.sequence_index = state.sequence_index;
    info.phase_id = currentPhaseToken();
    info.region_id = currentRegionId();
    info.kind = kind;
    info.is_partial_wait = partial_wait;
    info.is_taskgroup_end = taskgroup_end;
    m_wait_boundary_infos.push_back(info);
    return info;
  };
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = api->getCallee(call);
      ThreadAPI::TD_TYPE type = api->getType(callee);
      ThreadAPI::RuntimeLibrary library = api->getRuntimeLibrary(callee);
      bool is_nowait_variant = callee && callee->getName().contains("nowait");

      if (library == ThreadAPI::RuntimeLibrary::OpenMP && callee) {
        StringRef callee_name = callee->getName();
        if (callee_name.startswith("__kmpc_doacross_wait")) {
          type = ThreadAPI::TD_OMP_DOACROSS_WAIT;
        } else if (callee_name.startswith("__kmpc_doacross_submit")) {
          type = ThreadAPI::TD_OMP_DOACROSS_SUBMIT;
        } else if (callee_name.startswith("__kmpc_doacross")) {
          type = ThreadAPI::TD_OMP_DOACROSS_INIT;
        }
      }

      if (library == ThreadAPI::RuntimeLibrary::OpenMP) {
        if (type == ThreadAPI::TD_FORK) {
          ++m_summary.parallel_region_count;
          size_t current_depth = state.region_stack.size() + 1;
          if (current_depth > m_nested_depth) {
            m_nested_depth = current_depth;
          }
          if (current_depth > m_summary.nested_parallelism_max_depth) {
            m_summary.nested_parallelism_max_depth = current_depth;
          }
          if (current_depth > 1) {
            ++m_summary.nested_parallelism_nested_regions;
          } else {
            ++m_summary.nested_parallelism_flat_regions;
          }
          m_region_nesting_depth[state.next_region_id - 1] = current_depth;
          if (const auto *fork_target =
                  dyn_cast_or_null<Function>(api->getForkedFun(call))) {
            TraversalState fork_state;
            fork_state.scheduling_context_id = m_next_scheduling_context_id++;
            fork_state.phase_stack.push_back(0);
            fork_state.anchor_inst = call;
            std::set<const Function *> nested_call_stack;
            scanSchedulingContext(fork_target, fork_state, nested_call_stack);
          }
          continue;
        }

        if (type == ThreadAPI::TD_BAR_WAIT) {
          ++m_summary.barrier_count;
          WaitBoundary boundary;
          boundary.inst = call;
          boundary.scheduling_context_id = state.scheduling_context_id;
          boundary.sequence_index = state.sequence_index;
          boundary.sibling_group = currentPhaseToken();
          boundary.kind = WaitBoundaryInfo::Kind::Barrier;
          m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
          recordBoundary(call, WaitBoundaryInfo::Kind::Barrier);
          ++m_summary.wait_boundary_count;
          advanceCurrentPhase();
          continue;
        }

        if (type == ThreadAPI::TD_OMP_TASKYIELD) {
          ++m_summary.taskyield_count;
          continue;
        }

        if (type == ThreadAPI::TD_ACQUIRE || type == ThreadAPI::TD_RELEASE ||
            type == ThreadAPI::TD_TRY_ACQUIRE) {
          if (api->semanticTagStartsWith(callee, "critical")) {
            if (type == ThreadAPI::TD_ACQUIRE) {
              ++m_summary.critical_region_count;
            }
          } else {
            ++m_summary.lock_api_count;
          }
          continue;
        }

        if (type == ThreadAPI::TD_OMP_CANCEL) {
          if (api->hasSemanticTag(callee, "cancellation-point")) {
            ++m_summary.cancellation_point_count;
          } else {
            ++m_summary.cancel_count;
          }
          continue;
        }
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT_DEPS) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.is_partial_wait = true;
        boundary.kind = WaitBoundaryInfo::Kind::TaskwaitDeps;
        boundary.dependencies = extractRuntimeDependencies(call, 2, 3);
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        recordBoundary(call, WaitBoundaryInfo::Kind::TaskwaitDeps,
                       boundary.dependencies.empty());
        ++m_summary.partial_wait_boundary_count;
        if (boundary.dependencies.empty()) {
          ++m_deferred_wait_deps_count;
          ++m_deferred_reason_counts["omp_taskwait_deps_partial"];
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.kind = WaitBoundaryInfo::Kind::Taskwait;
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        recordBoundary(call, WaitBoundaryInfo::Kind::Taskwait);
        ++m_summary.wait_boundary_count;
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        state.taskgroup_stack.push_back(state.next_taskgroup_id++);
        state.phase_stack.push_back(state.next_phase_token++);
        pushRegion(WaitBoundaryInfo::Kind::TaskgroupEnd);
        ++m_summary.taskgroup_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.is_taskgroup_end = true;
        boundary.kind = WaitBoundaryInfo::Kind::TaskgroupEnd;
        WaitBoundaryInfo info = recordBoundary(
            call, WaitBoundaryInfo::Kind::TaskgroupEnd, false, true);
        if (!state.taskgroup_stack.empty()) {
          boundary.taskgroup_id = state.taskgroup_stack.back();
          info.taskgroup_id = boundary.taskgroup_id;
          state.taskgroup_stack.pop_back();
        }
        info.region_id = popRegion(WaitBoundaryInfo::Kind::TaskgroupEnd);
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        m_wait_boundary_infos.back() = info;
        ++m_summary.wait_boundary_count;
        if (!state.phase_stack.empty()) {
          state.phase_stack.pop_back();
        }
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd);
        ++m_summary.single_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_MASTER_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd);
        ++m_summary.master_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd);
        ++m_summary.ordered_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        pushRegion(type == ThreadAPI::TD_OMP_FOR_STATIC_INIT
                       ? WaitBoundaryInfo::Kind::ForFini
                       : WaitBoundaryInfo::Kind::DispatchFini);
        ++m_summary.worksharing_loop_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown);
        ++m_summary.reduction_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SECTIONS_INIT) {
        ++m_summary.sections_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ATOMIC_START) {
        ++m_summary.atomic_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET) {
        ++m_summary.target_region_count;
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.kind = is_nowait_variant ? WaitBoundaryInfo::Kind::TargetNowait
                                          : WaitBoundaryInfo::Kind::Target;
        boundary.is_partial_wait = is_nowait_variant;
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        recordBoundary(call, boundary.kind, boundary.is_partial_wait);
        ++m_summary.wait_boundary_count;
        if (is_nowait_variant) {
          ++m_summary.target_nowait_boundary_count;
          ++m_deferred_reason_counts["omp_target_nowait_partial"];
        } else {
          advanceCurrentPhase();
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TARGET_DATA_BEGIN ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_END ||
          type == ThreadAPI::TD_OMP_TARGET_DATA_UPDATE) {
        ++m_summary.target_data_region_count;
        if (type == ThreadAPI::TD_OMP_TARGET_DATA_END ||
            type == ThreadAPI::TD_OMP_TARGET_DATA_UPDATE) {
          WaitBoundary boundary;
          boundary.inst = call;
          boundary.scheduling_context_id = state.scheduling_context_id;
          boundary.sequence_index = state.sequence_index;
          boundary.sibling_group = currentPhaseToken();
          boundary.kind = is_nowait_variant
                              ? WaitBoundaryInfo::Kind::TargetDataNowait
                              : WaitBoundaryInfo::Kind::TargetData;
          boundary.is_partial_wait = is_nowait_variant;
          m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
          recordBoundary(call, boundary.kind, boundary.is_partial_wait);
          ++m_summary.wait_boundary_count;
          if (is_nowait_variant) {
            ++m_summary.target_nowait_boundary_count;
            ++m_deferred_reason_counts["omp_target_data_nowait_partial"];
          } else {
            advanceCurrentPhase();
          }
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_INIT) {
        ++m_summary.doacross_init_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_WAIT) {
        ++m_summary.doacross_wait_count;
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.is_partial_wait = true;
        boundary.kind = WaitBoundaryInfo::Kind::DoacrossWait;
        if (call->arg_size() >= 3) {
          const Value *witness = stripValue(call->getArgOperand(2));
          if (witness) {
            Dependency dep;
            dep.address = witness;
            dep.type = DependType::INOUT;
            dep.size = 0;
            dep.source_kind = DependencySourceKind::Iterator;
            dep.proof = DependencyProof::Possible;
            int64_t offset = 0;
            bool precise = false;
            dep.canonical_base = canonicalizeDependencyAddress(
                witness, m_module.getDataLayout(), offset, precise);
            dep.offset = offset;
            dep.has_precise_offset = precise;
            boundary.dependencies.push_back(dep);
          }
        }
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        recordBoundary(call, WaitBoundaryInfo::Kind::DoacrossWait, true);
        ++m_summary.wait_boundary_count;
        if (boundary.dependencies.empty()) {
          ++m_deferred_reason_counts["omp_doacross_partial"];
        }
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DOACROSS_SUBMIT) {
        ++m_summary.doacross_submit_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TASK_COMPLETE) {
        ++m_summary.detach_completion_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_TEAMS ||
          type == ThreadAPI::TD_OMP_TEAMS_HOST ||
          type == ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE) {
        ++m_summary.teams_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_DISTRIBUTE ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_STATIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC ||
          type == ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE) {
        ++m_summary.distribute_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_LOOP_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT ||
          type == ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT) {
        ++m_summary.loop_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_AFFINITY) {
        ++m_summary.affinity_region_count;
        continue;
      }
      if (type == ThreadAPI::TD_OMP_SCOPE_START ||
          type == ThreadAPI::TD_OMP_SCOPE_END) {
        ++m_summary.scope_region_count;
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_END ||
          type == ThreadAPI::TD_OMP_MASTER_END ||
          type == ThreadAPI::TD_OMP_ORDERED_END ||
          type == ThreadAPI::TD_OMP_SECTIONS_END ||
          type == ThreadAPI::TD_OMP_FOR_STATIC_FINI ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI ||
          type == ThreadAPI::TD_OMP_REDUCE_START) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);

        WaitBoundaryInfo::Kind kind = WaitBoundaryInfo::Kind::Unknown;
        if (type == ThreadAPI::TD_OMP_SINGLE_END) {
          kind = WaitBoundaryInfo::Kind::SingleEnd;
        } else if (type == ThreadAPI::TD_OMP_MASTER_END) {
          kind = WaitBoundaryInfo::Kind::SingleEnd;
        } else if (type == ThreadAPI::TD_OMP_ORDERED_END) {
          kind = WaitBoundaryInfo::Kind::SingleEnd;
        } else if (type == ThreadAPI::TD_OMP_SECTIONS_END) {
          kind = WaitBoundaryInfo::Kind::SectionsEnd;
        } else if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI) {
          kind = WaitBoundaryInfo::Kind::ForFini;
        } else if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
          kind = WaitBoundaryInfo::Kind::DispatchFini;
        } else if (type == ThreadAPI::TD_OMP_REDUCE_START) {
          kind = WaitBoundaryInfo::Kind::Reduce;
        }
        m_wait_boundaries[state.scheduling_context_id].back().kind = kind;
        WaitBoundaryInfo info = recordBoundary(call, kind);
        info.region_id = popRegion(kind);
        m_wait_boundary_infos.back() = info;
        ++m_summary.wait_boundary_count;
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_REDUCE_END ||
          type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END ||
          type == ThreadAPI::TD_OMP_FLUSH) {
        const char *reason = nullptr;
        if (type == ThreadAPI::TD_OMP_FLUSH) {
          ++m_summary.flush_count;
          WaitBoundary boundary;
          boundary.inst = call;
          boundary.scheduling_context_id = state.scheduling_context_id;
          boundary.sequence_index = state.sequence_index;
          boundary.sibling_group = currentPhaseToken();
          boundary.is_partial_wait = true;
          boundary.kind = WaitBoundaryInfo::Kind::Flush;
          if (call->arg_size() >= 1) {
            const Value *flush_obj = stripValue(call->getArgOperand(0));
            if (flush_obj) {
              Dependency dep;
              dep.address = flush_obj;
              dep.type = DependType::INOUT;
              dep.size = 0;
              dep.source_kind = DependencySourceKind::DirectAddress;
              dep.proof = DependencyProof::Possible;
              int64_t offset = 0;
              bool precise = false;
              dep.canonical_base = canonicalizeDependencyAddress(
                  flush_obj, m_module.getDataLayout(), offset, precise);
              dep.offset = offset;
              dep.has_precise_offset = precise;
              boundary.dependencies.push_back(dep);
            }
          }
          m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
          if (boundary.dependencies.empty()) {
            reason = "omp_flush_witness_required";
            ++m_deferred_reason_counts[reason];
          }
        } else {
          reason = "omp_region_boundary_observed";
          ++m_deferred_reason_counts[reason];
        }
        if (type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END) {
          popRegion(WaitBoundaryInfo::Kind::Unknown);
          WaitBoundary boundary;
          boundary.inst = call;
          boundary.scheduling_context_id = state.scheduling_context_id;
          boundary.sequence_index = state.sequence_index;
          boundary.sibling_group = currentPhaseToken();
          boundary.is_partial_wait = true;
          boundary.kind = WaitBoundaryInfo::Kind::ReduceNowait;
          m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
          recordBoundary(call, WaitBoundaryInfo::Kind::ReduceNowait, true);
          ++m_summary.wait_boundary_count;
          ++m_summary.reduction_nowait_boundary_count;
          ++m_deferred_reason_counts["omp_reduction_nowait_partial"];
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASK ||
          type == ThreadAPI::TD_OMP_TASKLOOP) {
        auto task = std::make_unique<Task>();
        task->task_create = call;
        task->task_function = extractTaskFunction(call);
        if (callee && callee->hasName()) {
          StringRef callee_name = callee->getName();
          if (callee_name.equals("__kmpc_omp_task_begin_if0")) {
            task->execution_mode = TaskExecutionMode::Included;
          }
        }
        task->parent_context = func;
        task->generating_context = state.anchor_inst ? state.anchor_inst : call;
        task->scheduling_context_id = state.scheduling_context_id;
        task->taskgroup_id =
            state.taskgroup_stack.empty() ? 0 : state.taskgroup_stack.back();
        task->phase_id = currentPhaseToken();
        task->sibling_group = currentPhaseToken();
        task->sequence_index = state.sequence_index++;
        task->region_id = currentRegionId();

        if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS) {
          task->dependencies = extractDependencies(call);
          ++m_summary.task_with_dependencies_count;
          for (const Dependency &dep : task->dependencies) {
            if (dep.canonical_base) {
              task->synchronization_objects.insert(dep.canonical_base);
            }
          }
        }
        if (type == ThreadAPI::TD_OMP_TASKLOOP) {
          ++m_summary.taskloop_count;
        }
        if (task->execution_mode == TaskExecutionMode::Included) {
          ++m_summary.included_task_count;
        }
        if (task->execution_mode == TaskExecutionMode::Final) {
          ++m_summary.final_task_count;
        }
        if (task->execution_mode == TaskExecutionMode::Untied) {
          ++m_summary.untied_task_count;
        }
        if (task->execution_mode == TaskExecutionMode::Detached) {
          ++m_summary.detached_task_count;
        }

        m_inst_to_task[call] = task.get();
        m_tasks.push_back(std::move(task));
        continue;
      }

      if (callee && !callee->isDeclaration() && type == ThreadAPI::TD_DUMMY &&
          !OpenMPModel::isOpenMP(callee->getName())) {
        const Instruction *saved_anchor = state.anchor_inst;
        state.anchor_inst = call;
        scanSchedulingContext(callee, state, call_stack);
        state.anchor_inst = saved_anchor;
      }
    }
  }

  call_stack.erase(func);
}

const Task *OpenMPTaskGraph::getTaskForCreate(const Instruction *inst) const {
  auto it = m_inst_to_task.find(inst);
  return it != m_inst_to_task.end() ? it->second : nullptr;
}

namespace {

const Value *resolveDependencyListValue(const Value *value) {
  if (!value) {
    return nullptr;
  }
  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getValueOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (isa<AllocaInst>(current) || isa<GlobalVariable>(current) ||
        isa<Argument>(current)) {
      bool saw_stored_pointer = false;
      for (const Use &use : current->uses()) {
        if (const auto *store = dyn_cast<StoreInst>(use.getUser())) {
          if (store->getPointerOperand()->stripPointerCasts() == current) {
            worklist.push_back(store->getValueOperand());
            saw_stored_pointer = true;
          }
        }
      }
      if (saw_stored_pointer) {
        continue;
      }
    }

    const Value *underlying = getUnderlyingObject(current);
    if (underlying) {
      current = underlying->stripPointerCasts();
    }
    if (!resolved) {
      resolved = current;
    } else if (resolved != current) {
      return nullptr;
    }
  }

  return resolved ? resolved : value->stripPointerCasts();
}

} // namespace

std::vector<Dependency>
OpenMPTaskGraph::extractDependencies(const CallBase *task_call) {
  return extractRuntimeDependencies(task_call, 3, 4);
}

std::vector<Dependency> OpenMPTaskGraph::extractRuntimeDependencies(
    const CallBase *task_call, unsigned ndeps_arg_idx, unsigned dep_arg_idx) {
  std::vector<Dependency> deps;
  const DataLayout &DL = m_module.getDataLayout();

  // OpenMP task_with_deps encoding:
  // __kmpc_omp_task_with_deps(ident_t*, kmp_int32 gtid, kmp_task_t* task,
  //                           kmp_int32 ndeps, kmp_depend_info_t* dep_list,
  //                           ...)
  //
  // kmp_depend_info_t contains:
  //   - base_addr: void*
  //   - len: size_t
  //   - flags: unsigned char (0x1=IN, 0x2=OUT, 0x3=INOUT)

  if (!task_call ||
      task_call->arg_size() <= std::max(ndeps_arg_idx, dep_arg_idx)) {
    return deps; // Not enough arguments
  }

  // Number of dependencies
  const Value *ndeps_val = task_call->getArgOperand(ndeps_arg_idx);
  const ConstantInt *CI = dyn_cast<ConstantInt>(ndeps_val);
  if (!CI) {
    return deps;
  }
  uint64_t ndeps = CI->getZExtValue();

  // Dependency list pointer
  const Value *dep_list = task_call->getArgOperand(dep_arg_idx);
  const Value *dep_root = resolveDependencyListValue(dep_list);
  const Value *dep_base =
      dep_root ? getUnderlyingObject(dep_root->stripPointerCasts()) : nullptr;
  if (!dep_base && dep_root) {
    dep_base = dep_root->stripPointerCasts();
  }

  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(dep_base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands() && deps.size() < ndeps;
             ++i) {
          Dependency dep;
          if (decodeConstantDependency(dyn_cast<Constant>(array->getOperand(i)),
                                       dep)) {
            dep.source_kind = gv->isConstant()
                                  ? DependencySourceKind::DirectAddress
                                  : DependencySourceKind::RegionSummary;
            dep.proof = gv->isConstant() ? DependencyProof::Definite
                                         : DependencyProof::Possible;
            dep.canonical_base = canonicalizeDependencyAddress(
                dep.address, DL, dep.offset, dep.has_precise_offset);
            deps.push_back(dep);
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast_or_null<AllocaInst>(dep_base)) {
    struct PartialDependency {
      const Value *address = nullptr;
      uint64_t size = 0;
      uint64_t flags = 0;
      DependencySourceKind source_kind = DependencySourceKind::RegionSummary;
      DependencyProof proof = DependencyProof::Unknown;
      bool has_address = false;
      bool has_size = false;
      bool has_flags = false;
    };

    std::map<uint64_t, PartialDependency> partials;
    const Function *parent = alloca->getFunction();
    if (!parent) {
      return deps;
    }
    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        continue;
      }
      const auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
      if (!gep || getUnderlyingObject(gep->getPointerOperand()) != alloca) {
        continue;
      }
      if (gep->getNumIndices() < 2) {
        continue;
      }

      SmallVector<unsigned, 4> indices;
      bool all_constant = true;
      for (unsigned i = 0; i < gep->getNumIndices(); ++i) {
        const auto *ci = dyn_cast<ConstantInt>(gep->getOperand(i + 1));
        if (!ci) {
          all_constant = false;
          break;
        }
        indices.push_back(ci->getZExtValue());
      }
      if (!all_constant) {
        continue;
      }

      uint64_t dep_idx = indices[indices.size() - 2];
      unsigned field_idx = indices.back();
      if (dep_idx >= ndeps) {
        continue;
      }

      PartialDependency &partial = partials[dep_idx];
      const Value *stored = store->getValueOperand();
      if (field_idx == 0) {
        partial.address = stripValue(stored);
        partial.has_address = partial.address != nullptr;
        auto evidence = classifyDependencyAddressEvidence(stored);
        partial.source_kind = evidence.first;
        partial.proof = evidence.second;
      } else if (field_idx == 1) {
        if (const auto *len = dyn_cast<ConstantInt>(stored)) {
          partial.size = len->getZExtValue();
          partial.has_size = true;
        }
      } else if (field_idx == 2) {
        if (const auto *flags = dyn_cast<ConstantInt>(stored)) {
          partial.flags = flags->getZExtValue();
          partial.has_flags = true;
        }
      }
    }

    for (uint64_t i = 0; i < ndeps; ++i) {
      auto it = partials.find(i);
      if (it == partials.end() || !it->second.has_address) {
        continue;
      }
      Dependency dep;
      dep.address = it->second.address;
      dep.size = it->second.has_size ? it->second.size : 0;
      dep.type =
          decodeDependType(it->second.has_flags ? it->second.flags : 0x3);
      dep.source_kind = it->second.source_kind;
      dep.proof =
          it->second.has_address ? it->second.proof : DependencyProof::Unknown;
      dep.canonical_base = canonicalizeDependencyAddress(
          dep.address, DL, dep.offset, dep.has_precise_offset);
      deps.push_back(dep);
    }
  }

  return deps;
}

const Function *
OpenMPTaskGraph::extractTaskFunction(const CallBase *task_call) {
  if (!task_call || task_call->arg_size() < 3) {
    return nullptr;
  }

  const Value *task_arg = task_call->getArgOperand(2)->stripPointerCasts();
  if (const auto *direct = dyn_cast<Function>(task_arg)) {
    return direct;
  }

  const Value *task_base = getUnderlyingObject(task_arg);
  if (!task_base) {
    task_base = task_arg;
  }

  auto tryStoredFunction = [&](const Value *value) -> const Function * {
    if (!value) {
      return nullptr;
    }
    value = value->stripPointerCasts();
    if (const auto *func = dyn_cast<Function>(value)) {
      return func;
    }
    if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
      if (ce->isCast()) {
        return dyn_cast<Function>(ce->getOperand(0)->stripPointerCasts());
      }
    }
    return nullptr;
  };

  auto pointerTargetsTask = [&](const Value *ptr) {
    if (!ptr) {
      return false;
    }
    ptr = ptr->stripPointerCasts();
    if (ptr == task_arg || ptr == task_base) {
      return true;
    }
    if (const Value *underlying = getUnderlyingObject(ptr)) {
      return underlying->stripPointerCasts() == task_base;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(ptr)) {
      return getUnderlyingObject(gep->getPointerOperand()) == task_base;
    }
    return false;
  };

  const Function *parent = task_call->getFunction();
  if (!parent) {
    return nullptr;
  }
  for (const Instruction &inst : instructions(parent)) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store || !pointerTargetsTask(store->getPointerOperand())) {
      continue;
    }
    if (const Function *stored = tryStoredFunction(store->getValueOperand())) {
      return stored;
    }
  }
  return nullptr;
}

void OpenMPTaskGraph::buildDependencyEdges() {
  // Build happens-before edges based on task dependencies

  auto recordRelation =
      [&](const Task *lhs, const Task *rhs, concurrency::RelationKind kind,
          concurrency::ProofStrength proof, StringRef reason) {
        if (!lhs || !rhs || lhs == rhs) {
          return;
        }
        auto key = normalizeTaskPair(lhs, rhs);
        concurrency::Relation relation;
        relation.kind = kind;
        relation.proof = proof;
        relation.reason = reason.str();

        auto it = m_relations.find(key);
        if (it == m_relations.end() ||
            concurrency::relationPriority(kind) >
                concurrency::relationPriority(it->second.kind)) {
          m_relations[key] = std::move(relation);
        }
      };

  auto crossesDeferredPartialWaitBoundary = [&](const Task *lhs,
                                                const Task *rhs) -> bool {
    if (!lhs || !rhs ||
        lhs->scheduling_context_id != rhs->scheduling_context_id) {
      return false;
    }
    const size_t earlier = std::min(lhs->sequence_index, rhs->sequence_index);
    const size_t later = std::max(lhs->sequence_index, rhs->sequence_index);
    for (const auto &entry : m_wait_boundaries) {
      if (entry.first != lhs->scheduling_context_id) {
        continue;
      }
      for (const WaitBoundary &boundary : entry.second) {
        if (!boundary.is_partial_wait || !boundary.dependencies.empty()) {
          continue;
        }
        if (earlier < boundary.sequence_index &&
            boundary.sequence_index <= later) {
          return true;
        }
      }
    }
    return false;
  };

  for (size_t i = 0; i < m_tasks.size(); ++i) {
    Task *task_i = m_tasks[i].get();

    for (size_t j = i + 1; j < m_tasks.size(); ++j) {
      Task *task_j = m_tasks[j].get();
      if (task_i->scheduling_context_id != task_j->scheduling_context_id) {
        continue;
      }
      if (crossesDeferredPartialWaitBoundary(task_i, task_j)) {
        recordRelation(
            task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
            concurrency::ProofStrength::Unknown, "omp_taskwait_deps_partial");
        continue;
      }
      // Check if tasks have conflicting dependencies
      bool saw_conflict = false;
      bool saw_mutex_exclusion = false;
      bool saw_unknown_conflict = false;
      for (const Dependency &dep_i : task_i->dependencies) {
        for (const Dependency &dep_j : task_j->dependencies) {
          DependencyConflict conflict =
              classifyDependencyConflict(dep_i, dep_j);
          if (conflict == DependencyConflict::MustConflict) {
            saw_conflict = true;
            saw_mutex_exclusion =
                saw_mutex_exclusion || isMutexLikeExclusion(dep_i, dep_j);
          } else if (conflict == DependencyConflict::MayConflict ||
                     conflict == DependencyConflict::Unknown) {
            saw_unknown_conflict = true;
          }
        }
      }

      if (!saw_conflict) {
        if (saw_unknown_conflict) {
          recordRelation(
              task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
              concurrency::ProofStrength::May, "omp_depend_may_conflict");
        }
        continue;
      }

      if (saw_mutex_exclusion) {
        task_i->exclusions.insert(task_j);
        task_j->exclusions.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MutuallyExclusive,
                       concurrency::ProofStrength::Must, "omp_mutexinoutset");
        continue;
      }

      if (mustHappenBefore(taskOrderingSite(task_i),
                           taskOrderingSite(task_j))) {
        task_i->successors.insert(task_j);
        task_j->predecessors.insert(task_i);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else if (mustHappenBefore(taskOrderingSite(task_j),
                                  taskOrderingSite(task_i))) {
        task_j->successors.insert(task_i);
        task_i->predecessors.insert(task_j);
        recordRelation(task_i, task_j,
                       concurrency::RelationKind::MustHappenBefore,
                       concurrency::ProofStrength::Must, "omp_depend_ordered");
      } else {
        recordRelation(
            task_i, task_j, concurrency::RelationKind::UnknownDueToModelGap,
            concurrency::ProofStrength::Unknown, "omp_nonlexical_task_order");
      }
    }
  }

  for (const auto &entry : m_wait_boundaries) {
    for (const WaitBoundary &boundary : entry.second) {
      for (const auto &lhs : m_tasks) {
        if (lhs->scheduling_context_id != boundary.scheduling_context_id ||
            lhs->sequence_index >= boundary.sequence_index) {
          continue;
        }
        if (boundary.is_taskgroup_end &&
            (boundary.taskgroup_id == 0 ||
             lhs->taskgroup_id != boundary.taskgroup_id)) {
          continue;
        }
        if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
            lhs->phase_id != boundary.sibling_group) {
          continue;
        }
        for (const auto &rhs : m_tasks) {
          if (rhs->scheduling_context_id != boundary.scheduling_context_id ||
              rhs.get() == lhs.get() ||
              rhs->sequence_index < boundary.sequence_index) {
            continue;
          }
          if (!boundary.is_taskgroup_end && !boundary.is_partial_wait &&
              rhs->phase_id <= boundary.sibling_group) {
            continue;
          }
          if (boundary.is_partial_wait) {
            bool lhs_selected = false;
            bool rhs_selected = false;
            for (const Dependency &wait_dep : boundary.dependencies) {
              for (const Dependency &lhs_dep : lhs->dependencies) {
                if (classifyDependencyConflict(lhs_dep, wait_dep) ==
                    DependencyConflict::MustConflict) {
                  lhs_selected = true;
                  break;
                }
              }
              for (const Dependency &rhs_dep : rhs->dependencies) {
                if (classifyDependencyConflict(rhs_dep, wait_dep) !=
                    DependencyConflict::NoConflict) {
                  rhs_selected = true;
                  break;
                }
              }
              if (lhs_selected && rhs_selected) {
                break;
              }
            }
            StringRef selective_reason = "omp_taskwait_deps_selective";
            StringRef deferred_reason = "omp_taskwait_deps_partial";
            if (boundary.kind == WaitBoundaryInfo::Kind::Flush) {
              selective_reason = "omp_flush_selective";
              deferred_reason = "omp_flush_witness_required";
            } else if (boundary.kind == WaitBoundaryInfo::Kind::DoacrossWait) {
              selective_reason = "omp_doacross_selective";
              deferred_reason = "omp_doacross_partial";
            } else if (boundary.kind == WaitBoundaryInfo::Kind::TargetNowait) {
              selective_reason = "omp_target_nowait_selective";
              deferred_reason = "omp_target_nowait_partial";
            } else if (boundary.kind ==
                       WaitBoundaryInfo::Kind::TargetDataNowait) {
              selective_reason = "omp_target_data_nowait_selective";
              deferred_reason = "omp_target_data_nowait_partial";
            } else if (boundary.kind == WaitBoundaryInfo::Kind::ReduceNowait) {
              selective_reason = "omp_reduction_nowait_selective";
              deferred_reason = "omp_reduction_nowait_partial";
            }
            if (!boundary.dependencies.empty() && lhs_selected &&
                rhs_selected &&
                mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
                mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
              lhs->successors.insert(rhs.get());
              rhs->predecessors.insert(lhs.get());
              recordRelation(lhs.get(), rhs.get(),
                             concurrency::RelationKind::SelectiveHappenBefore,
                             concurrency::ProofStrength::Must,
                             selective_reason);
            } else {
              ++m_deferred_reason_counts[deferred_reason.str()];
              recordRelation(lhs.get(), rhs.get(),
                             concurrency::RelationKind::UnknownDueToModelGap,
                             concurrency::ProofStrength::Unknown,
                             deferred_reason);
            }
            continue;
          }
          if (mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
              mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
            lhs->successors.insert(rhs.get());
            rhs->predecessors.insert(lhs.get());
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::MustHappenBefore,
                           concurrency::ProofStrength::Must,
                           "omp_wait_boundary");
          } else {
            recordRelation(lhs.get(), rhs.get(),
                           concurrency::RelationKind::UnknownDueToModelGap,
                           concurrency::ProofStrength::Unknown,
                           "omp_conditional_wait_boundary");
          }
        }
      }
    }
  }
}

bool OpenMPTaskGraph::dependenciesConflict(const Dependency &d1,
                                           const Dependency &d2) const {
  return classifyDependencyConflict(d1, d2) == DependencyConflict::MustConflict;
}

DependencyConflict
OpenMPTaskGraph::classifyDependencyConflict(const Dependency &d1,
                                            const Dependency &d2) const {
  // Two dependencies conflict if:
  // 1. They access the same memory location (alias analysis needed)
  // 2. At least one is a write (OUT, INOUT, MUTEXINOUTSET)

  const DataLayout &DL = m_module.getDataLayout();
  int64_t offset1 = d1.offset;
  int64_t offset2 = d2.offset;
  bool precise1 = d1.has_precise_offset;
  bool precise2 = d2.has_precise_offset;
  const Value *base1 =
      d1.canonical_base
          ? d1.canonical_base
          : canonicalizeDependencyAddress(d1.address, DL, offset1, precise1);
  const Value *base2 =
      d2.canonical_base
          ? d2.canonical_base
          : canonicalizeDependencyAddress(d2.address, DL, offset2, precise2);

  if (!base1 || !base2) {
    return DependencyConflict::Unknown;
  }

  bool is_write1 =
      (d1.type == DependType::OUT || d1.type == DependType::INOUT ||
       d1.type == DependType::MUTEXINOUTSET);
  bool is_write2 =
      (d2.type == DependType::OUT || d2.type == DependType::INOUT ||
       d2.type == DependType::MUTEXINOUTSET);

  if (base1 != base2) {
    if ((is_write1 || is_write2) &&
        (d1.proof != DependencyProof::Definite ||
         d2.proof != DependencyProof::Definite ||
         d1.source_kind != DependencySourceKind::DirectAddress ||
         d2.source_kind != DependencySourceKind::DirectAddress)) {
      ++m_deferred_imprecise_conflict_count;
      ++m_deferred_reason_counts["omp_depend_distinct_base_may_alias"];
      return DependencyConflict::MayConflict;
    }
    return DependencyConflict::NoConflict;
  }

  if (!(is_write1 || is_write2)) {
    return DependencyConflict::NoConflict;
  }

  if (precise1 && precise2 && d1.size != 0 && d2.size != 0) {
    uint64_t begin1 = static_cast<uint64_t>(offset1);
    uint64_t begin2 = static_cast<uint64_t>(offset2);
    uint64_t end1 = begin1 + d1.size;
    uint64_t end2 = begin2 + d2.size;
    return begin1 < end2 && begin2 < end1 ? DependencyConflict::MustConflict
                                          : DependencyConflict::NoConflict;
  }

  if (stripValue(d1.address) &&
      stripValue(d1.address) == stripValue(d2.address)) {
    return DependencyConflict::MustConflict;
  }

  // Shared base but imprecise offsets are recognized as potential depend
  // conflicts. Keep them conservative instead of treating them as parallel.
  ++m_deferred_imprecise_conflict_count;
  ++m_deferred_reason_counts["omp_depend_may_conflict"];
  return DependencyConflict::MayConflict;
}

bool OpenMPTaskGraph::isMutexLikeExclusion(const Dependency &d1,
                                           const Dependency &d2) const {
  return d1.type == DependType::MUTEXINOUTSET ||
         d2.type == DependType::MUTEXINOUTSET;
}

bool OpenMPTaskGraph::happensBefore(const Task *t1, const Task *t2) const {
  // Check if t1 happens-before t2 via dependency graph

  if (!t1 || !t2 || t1 == t2) {
    return false;
  }

  // BFS through successors
  std::set<const Task *> visited;
  std::vector<const Task *> worklist;
  worklist.push_back(t1);
  visited.insert(t1);

  while (!worklist.empty()) {
    const Task *current = worklist.back();
    worklist.pop_back();

    if (current == t2) {
      return true;
    }

    for (const Task *succ : current->successors) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

size_t OpenMPTaskGraph::getRelationCount(concurrency::RelationKind kind) const {
  size_t count = 0;
  for (const auto &entry : m_relations) {
    if (entry.second.kind == kind) {
      ++count;
    }
  }
  return count;
}

bool OpenMPTaskGraph::isNestedRegion(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return false;
  }
  return it->second > 1;
}

size_t OpenMPTaskGraph::getRegionNestingDepth(size_t region_id) const {
  auto it = m_region_nesting_depth.find(region_id);
  if (it == m_region_nesting_depth.end()) {
    return 0;
  }
  return it->second;
}

OpenMPTaskGraph::TaskRelation
OpenMPTaskGraph::classifyTaskRelation(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return TaskRelation::Unknown;
  }
  if (happensBefore(t1, t2) || happensBefore(t2, t1)) {
    return TaskRelation::HappensBefore;
  }
  if (t1->exclusions.count(const_cast<Task *>(t2)) ||
      t2->exclusions.count(const_cast<Task *>(t1))) {
    return TaskRelation::Excluded;
  }
  if (m_relations.count(normalizeTaskPair(t1, t2))) {
    return TaskRelation::Unknown;
  }
  return TaskRelation::Parallel;
}

bool OpenMPTaskGraph::mayBeParallel(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return false;
  }
  return classifyTaskRelation(t1, t2) == TaskRelation::Parallel;
}
