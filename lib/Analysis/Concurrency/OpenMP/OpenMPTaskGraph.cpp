/**
 * @file OpenMPTaskGraph.cpp
 * @brief Implementation of OpenMP Task Dependency Graph
 */

#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"
#include "Analysis/Concurrency/OpenMP/OpenMPModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <limits>
#include <tuple>

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
  case 0x3ULL:
    return DependType::INOUT;
  default:
    return DependType::INOUT;
  }
}

const Value *stripValue(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
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

uint64_t extractArrayIndex(const GEPOperator *gep) {
  if (!gep) {
    return std::numeric_limits<uint64_t>::max();
  }
  uint64_t array_idx = std::numeric_limits<uint64_t>::max();
  for (unsigned i = 0; i < gep->getNumIndices(); ++i) {
    if (const auto *ci = dyn_cast<ConstantInt>(gep->getOperand(i + 1))) {
      array_idx = ci->getZExtValue();
    } else {
      return std::numeric_limits<uint64_t>::max();
    }
  }
  return array_idx;
}

unsigned extractFieldIndex(const GEPOperator *gep) {
  if (!gep || gep->getNumIndices() == 0) {
    return std::numeric_limits<unsigned>::max();
  }
  if (const auto *ci =
          dyn_cast<ConstantInt>(gep->getOperand(gep->getNumOperands() - 1))) {
    return ci->getZExtValue();
  }
  return std::numeric_limits<unsigned>::max();
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
  return task->generating_context ? task->generating_context : task->task_create;
}

} // namespace

OpenMPTaskGraph::OpenMPTaskGraph(Module &module) : m_module(module) {}

void OpenMPTaskGraph::analyze() {
  errs() << "Starting OpenMP Task Dependency Analysis...\n";
  m_tasks.clear();
  m_inst_to_task.clear();
  m_wait_boundaries.clear();
  m_wait_boundary_infos.clear();
  m_unknown_relations.clear();
  m_next_scheduling_context_id = 1;
  m_deferred_wait_deps_count = 0;
  m_deferred_imprecise_conflict_count = 0;
  m_deferred_reason_counts.clear();
  identifyTasks();
  buildDependencyEdges();
  
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

      if (type == ThreadAPI::TD_OMP_TASKWAIT_DEPS) {
        // wait_deps is a selective wait and does not imply full sibling-phase
        // ordering in the absence of precise dependency-object lowering.
        recordBoundary(call, WaitBoundaryInfo::Kind::TaskwaitDeps, true);
        ++m_deferred_wait_deps_count;
        ++m_deferred_reason_counts["omp_taskwait_deps_partial"];
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKWAIT) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        recordBoundary(call, WaitBoundaryInfo::Kind::Taskwait);
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        state.taskgroup_stack.push_back(state.next_taskgroup_id++);
        state.phase_stack.push_back(state.next_phase_token++);
        pushRegion(WaitBoundaryInfo::Kind::TaskgroupEnd);
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        WaitBoundary boundary;
        boundary.inst = call;
        boundary.scheduling_context_id = state.scheduling_context_id;
        boundary.sequence_index = state.sequence_index;
        boundary.sibling_group = currentPhaseToken();
        boundary.is_taskgroup_end = true;
        WaitBoundaryInfo info =
            recordBoundary(call, WaitBoundaryInfo::Kind::TaskgroupEnd, false, true);
        if (!state.taskgroup_stack.empty()) {
          boundary.taskgroup_id = state.taskgroup_stack.back();
          info.taskgroup_id = boundary.taskgroup_id;
          state.taskgroup_stack.pop_back();
        }
        info.region_id = popRegion(WaitBoundaryInfo::Kind::TaskgroupEnd);
        m_wait_boundaries[state.scheduling_context_id].push_back(boundary);
        m_wait_boundary_infos.back() = info;
        if (!state.phase_stack.empty()) {
          state.phase_stack.pop_back();
        }
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        pushRegion(WaitBoundaryInfo::Kind::SingleEnd);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_MASTER_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_ORDERED_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_FOR_STATIC_INIT ||
          type == ThreadAPI::TD_OMP_FOR_DISPATCH_INIT) {
        pushRegion(type == ThreadAPI::TD_OMP_FOR_STATIC_INIT
                       ? WaitBoundaryInfo::Kind::ForFini
                       : WaitBoundaryInfo::Kind::DispatchFini);
        continue;
      }
      if (type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_START) {
        pushRegion(WaitBoundaryInfo::Kind::Unknown);
        continue;
      }

      if (type == ThreadAPI::TD_OMP_SINGLE_END ||
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
        } else if (type == ThreadAPI::TD_OMP_SECTIONS_END) {
          kind = WaitBoundaryInfo::Kind::SectionsEnd;
        } else if (type == ThreadAPI::TD_OMP_FOR_STATIC_FINI) {
          kind = WaitBoundaryInfo::Kind::ForFini;
        } else if (type == ThreadAPI::TD_OMP_FOR_DISPATCH_FINI) {
          kind = WaitBoundaryInfo::Kind::DispatchFini;
        } else if (type == ThreadAPI::TD_OMP_REDUCE_START) {
          kind = WaitBoundaryInfo::Kind::Reduce;
        }
        WaitBoundaryInfo info = recordBoundary(call, kind);
        info.region_id = popRegion(kind);
        m_wait_boundary_infos.back() = info;
        advanceCurrentPhase();
        continue;
      }

      if (type == ThreadAPI::TD_OMP_MASTER_END ||
          type == ThreadAPI::TD_OMP_ORDERED_END ||
          type == ThreadAPI::TD_OMP_REDUCE_END ||
          type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END ||
          type == ThreadAPI::TD_OMP_FLUSH ||
          type == ThreadAPI::TD_OMP_CANCEL) {
        const char *reason = nullptr;
        if (type == ThreadAPI::TD_OMP_FLUSH) {
          reason = "omp_flush_witness_required";
        } else if (type == ThreadAPI::TD_OMP_CANCEL) {
          reason = "omp_cancel_unknown";
        } else if (type == ThreadAPI::TD_OMP_ORDERED_END) {
          reason = "omp_ordered_region_tracked";
        } else {
          reason = "omp_region_boundary_observed";
        }
        ++m_deferred_reason_counts[reason];
        if (type == ThreadAPI::TD_OMP_MASTER_END ||
            type == ThreadAPI::TD_OMP_ORDERED_END ||
            type == ThreadAPI::TD_OMP_REDUCE_NOWAIT_END) {
          popRegion(WaitBoundaryInfo::Kind::Unknown);
        }
        continue;
      }

      if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS ||
          type == ThreadAPI::TD_OMP_TASK ||
          type == ThreadAPI::TD_OMP_TASKLOOP) {
        auto task = std::make_unique<Task>();
        task->task_create = call;
        task->task_function = extractTaskFunction(call);
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
        }

        m_inst_to_task[call] = task.get();
        m_tasks.push_back(std::move(task));
        continue;
      }

      if (callee && !callee->isDeclaration() &&
          type == ThreadAPI::TD_DUMMY &&
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

std::vector<Dependency>
OpenMPTaskGraph::extractDependencies(const CallBase *task_call) {
  std::vector<Dependency> deps;
  const DataLayout &DL = m_module.getDataLayout();
  
  // OpenMP task_with_deps encoding:
  // __kmpc_omp_task_with_deps(ident_t*, kmp_int32 gtid, kmp_task_t* task,
  //                           kmp_int32 ndeps, kmp_depend_info_t* dep_list, ...)
  //
  // kmp_depend_info_t contains:
  //   - base_addr: void*
  //   - len: size_t  
  //   - flags: unsigned char (0x1=IN, 0x2=OUT, 0x3=INOUT)
  
  if (task_call->arg_size() < 5) {
    return deps; // Not enough arguments
  }
  
  // Number of dependencies
  const Value *ndeps_val = task_call->getArgOperand(3);
  const ConstantInt *CI = dyn_cast<ConstantInt>(ndeps_val);
  if (!CI) {
    return deps;
  }
  uint64_t ndeps = CI->getZExtValue();

  // Dependency list pointer
  const Value *dep_list = task_call->getArgOperand(4);
  const Value *dep_base = getUnderlyingObject(dep_list->stripPointerCasts());

  if (const auto *gv = dyn_cast_or_null<GlobalVariable>(dep_base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands() &&
                             deps.size() < ndeps; ++i) {
          Dependency dep;
          if (decodeConstantDependency(dyn_cast<Constant>(array->getOperand(i)),
                                       dep)) {
            dep.source_kind = DependencySourceKind::DirectAddress;
            dep.proof = DependencyProof::Definite;
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
      dep.type = decodeDependType(it->second.has_flags ? it->second.flags : 0x3);
      dep.source_kind = DependencySourceKind::RegionSummary;
      dep.proof = it->second.has_address ? DependencyProof::Possible
                                         : DependencyProof::Unknown;
      dep.canonical_base = canonicalizeDependencyAddress(
          dep.address, DL, dep.offset, dep.has_precise_offset);
      deps.push_back(dep);
    }
  }

  return deps;
}

const Function *OpenMPTaskGraph::extractTaskFunction(const CallBase *task_call) {
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

  auto crossesPartialWaitBoundary = [&](const Task *lhs,
                                        const Task *rhs) -> bool {
    if (!lhs || !rhs ||
        lhs->scheduling_context_id != rhs->scheduling_context_id) {
      return false;
    }
    const size_t earlier = std::min(lhs->sequence_index, rhs->sequence_index);
    const size_t later = std::max(lhs->sequence_index, rhs->sequence_index);
    for (const WaitBoundaryInfo &info : m_wait_boundary_infos) {
      if (!info.is_partial_wait ||
          info.scheduling_context_id != lhs->scheduling_context_id) {
        continue;
      }
      if (earlier < info.sequence_index && info.sequence_index <= later) {
        return true;
      }
    }
    return false;
  };

  auto recordUnknownRelation = [&](const Task *lhs, const Task *rhs) {
    if (!lhs || !rhs || lhs == rhs) {
      return;
    }
    m_unknown_relations.insert(normalizeTaskPair(lhs, rhs));
  };

  auto dependenciesMayConflictUnknown = [&](const Dependency &d1,
                                            const Dependency &d2) {
    const DataLayout &DL = m_module.getDataLayout();
    int64_t offset1 = d1.offset;
    int64_t offset2 = d2.offset;
    bool precise1 = d1.has_precise_offset;
    bool precise2 = d2.has_precise_offset;
    const Value *base1 = d1.canonical_base ? d1.canonical_base
                                           : canonicalizeDependencyAddress(
                                                 d1.address, DL, offset1, precise1);
    const Value *base2 = d2.canonical_base ? d2.canonical_base
                                           : canonicalizeDependencyAddress(
                                                 d2.address, DL, offset2, precise2);
    if (!base1 || !base2 || base1 != base2) {
      return false;
    }

    bool is_write1 = (d1.type == DependType::OUT ||
                      d1.type == DependType::INOUT ||
                      d1.type == DependType::MUTEXINOUTSET);
    bool is_write2 = (d2.type == DependType::OUT ||
                      d2.type == DependType::INOUT ||
                      d2.type == DependType::MUTEXINOUTSET);
    if (!(is_write1 || is_write2)) {
      return false;
    }

    if (precise1 && precise2 && d1.size != 0 && d2.size != 0) {
      return false;
    }
    return !(stripValue(d1.address) &&
             stripValue(d1.address) == stripValue(d2.address));
  };

  for (size_t i = 0; i < m_tasks.size(); ++i) {
    Task *task_i = m_tasks[i].get();

    for (size_t j = i + 1; j < m_tasks.size(); ++j) {
      Task *task_j = m_tasks[j].get();
      if (task_i->scheduling_context_id != task_j->scheduling_context_id) {
        continue;
      }
      // __kmpc_omp_wait_deps synchronizes only a selected subset of tasks.
      // Without precise lowering of the dependency objects, we avoid adding a
      // definite HB edge across that boundary.
      if (crossesPartialWaitBoundary(task_i, task_j)) {
        recordUnknownRelation(task_i, task_j);
        continue;
      }

      // Check if tasks have conflicting dependencies
      bool saw_conflict = false;
      bool saw_mutex_exclusion = false;
      bool saw_unknown_conflict = false;
      for (const Dependency &dep_i : task_i->dependencies) {
        for (const Dependency &dep_j : task_j->dependencies) {
          if (dependenciesConflict(dep_i, dep_j)) {
            saw_conflict = true;
            saw_mutex_exclusion = saw_mutex_exclusion ||
                                  isMutexLikeExclusion(dep_i, dep_j);
          } else if (dependenciesMayConflictUnknown(dep_i, dep_j)) {
            saw_unknown_conflict = true;
          }
        }
      }

      if (!saw_conflict) {
        if (saw_unknown_conflict) {
          recordUnknownRelation(task_i, task_j);
        }
        continue;
      }

      if (saw_mutex_exclusion) {
        task_i->exclusions.insert(task_j);
        task_j->exclusions.insert(task_i);
        continue;
      }

      if (mustHappenBefore(taskOrderingSite(task_i), taskOrderingSite(task_j))) {
        task_i->successors.insert(task_j);
        task_j->predecessors.insert(task_i);
      } else if (mustHappenBefore(taskOrderingSite(task_j),
                                  taskOrderingSite(task_i))) {
        task_j->successors.insert(task_i);
        task_i->predecessors.insert(task_j);
      } else {
        recordUnknownRelation(task_i, task_j);
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
        if (!boundary.is_taskgroup_end &&
            lhs->phase_id != boundary.sibling_group) {
          continue;
        }
        for (const auto &rhs : m_tasks) {
          if (rhs->scheduling_context_id != boundary.scheduling_context_id ||
              rhs.get() == lhs.get() ||
              rhs->sequence_index < boundary.sequence_index) {
            continue;
          }
          if (!boundary.is_taskgroup_end &&
              rhs->phase_id <= boundary.sibling_group) {
            continue;
          }
          if (mustHappenBefore(taskOrderingSite(lhs.get()), boundary.inst) &&
              mustHappenBefore(boundary.inst, taskOrderingSite(rhs.get()))) {
            lhs->successors.insert(rhs.get());
            rhs->predecessors.insert(lhs.get());
          } else {
            recordUnknownRelation(lhs.get(), rhs.get());
          }
        }
      }
    }
  }
}

bool OpenMPTaskGraph::dependenciesConflict(const Dependency &d1, 
                                           const Dependency &d2) const {
  // Two dependencies conflict if:
  // 1. They access the same memory location (alias analysis needed)
  // 2. At least one is a write (OUT, INOUT, MUTEXINOUTSET)
  
  const DataLayout &DL = m_module.getDataLayout();
  int64_t offset1 = d1.offset;
  int64_t offset2 = d2.offset;
  bool precise1 = d1.has_precise_offset;
  bool precise2 = d2.has_precise_offset;
  const Value *base1 = d1.canonical_base ? d1.canonical_base
                                         : canonicalizeDependencyAddress(
                                               d1.address, DL, offset1, precise1);
  const Value *base2 = d2.canonical_base ? d2.canonical_base
                                         : canonicalizeDependencyAddress(
                                               d2.address, DL, offset2, precise2);

  if (!base1 || !base2 || base1 != base2) {
    return false;
  }
  
  // Check for write dependency
  bool is_write1 = (d1.type == DependType::OUT || 
                    d1.type == DependType::INOUT ||
                    d1.type == DependType::MUTEXINOUTSET);
  bool is_write2 = (d2.type == DependType::OUT || 
                    d2.type == DependType::INOUT ||
                    d2.type == DependType::MUTEXINOUTSET);
  
  if (!(is_write1 || is_write2)) {
    return false;
  }

  if (precise1 && precise2 && d1.size != 0 && d2.size != 0) {
    uint64_t begin1 = static_cast<uint64_t>(offset1);
    uint64_t begin2 = static_cast<uint64_t>(offset2);
    uint64_t end1 = begin1 + d1.size;
    uint64_t end2 = begin2 + d2.size;
    return begin1 < end2 && begin2 < end1;
  }

  if (stripValue(d1.address) && stripValue(d1.address) == stripValue(d2.address)) {
    return true;
  }

  // Shared base but imprecise offsets are recognized as potential depend
  // conflicts, but we do not create a definite HB edge without a provable
  // overlap witness.
  ++m_deferred_imprecise_conflict_count;
  return false;
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
  if (m_unknown_relations.count(normalizeTaskPair(t1, t2))) {
    return TaskRelation::Unknown;
  }
  return TaskRelation::Parallel;
}

bool OpenMPTaskGraph::mayBeParallel(const Task *t1, const Task *t2) const {
  if (!t1 || !t2 || t1 == t2) {
    return false;
  }
  if (t1->exclusions.count(const_cast<Task *>(t2)) ||
      t2->exclusions.count(const_cast<Task *>(t1))) {
    return false;
  }
  return !happensBefore(t1, t2) && !happensBefore(t2, t1);
}
