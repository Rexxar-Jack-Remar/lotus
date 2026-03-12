/**
 * @file OpenMPTaskGraph.cpp
 * @brief Implementation of OpenMP Task Dependency Graph
 */

#include "Analysis/Concurrency/Utils/OpenMPTaskGraph.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <limits>
#include <tuple>

using namespace llvm;
using namespace OpenMP;

namespace {

DependType decodeDependType(uint64_t flags) {
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

} // namespace

OpenMPTaskGraph::OpenMPTaskGraph(Module &module) : m_module(module) {}

void OpenMPTaskGraph::analyze() {
  errs() << "Starting OpenMP Task Dependency Analysis...\n";
  m_tasks.clear();
  m_inst_to_task.clear();
  m_wait_boundaries.clear();
  identifyTasks();
  buildDependencyEdges();
  
  errs() << "Found " << m_tasks.size() << " OpenMP tasks with dependencies\n";
}

void OpenMPTaskGraph::identifyTasks() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  
  for (Function &func : m_module) {
    size_t sequence_index = 0;
    size_t next_taskgroup_id = 1;
    std::vector<size_t> taskgroup_stack;
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallBase *call = dyn_cast<CallBase>(&inst)) {
          // Check for __kmpc_omp_task_with_deps or __kmpc_omp_task
          ThreadAPI::TD_TYPE type = api->getType(api->getCallee(call));

          if (type == ThreadAPI::TD_OMP_TASKWAIT) {
            m_wait_boundaries[&func].push_back(sequence_index);
            continue;
          }

          if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
            taskgroup_stack.push_back(next_taskgroup_id++);
            continue;
          }

          if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
            m_wait_boundaries[&func].push_back(sequence_index);
            if (!taskgroup_stack.empty()) {
              taskgroup_stack.pop_back();
            }
            continue;
          }

          if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS || 
              type == ThreadAPI::TD_OMP_TASK) {
            auto task = std::make_unique<Task>();
            task->task_create = call;
            task->task_function = nullptr;
            task->parent_context = &func;
            task->taskgroup_id = taskgroup_stack.empty() ? 0 : taskgroup_stack.back();
            task->sequence_index = sequence_index++;
            
            // Extract dependencies if this is a task_with_deps
            if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS) {
              task->dependencies = extractDependencies(call);
            }
            
            m_inst_to_task[call] = task.get();
            m_tasks.push_back(std::move(task));
          }
        }

      }
    }
  }
}

std::vector<Dependency>
OpenMPTaskGraph::extractDependencies(const CallBase *task_call) {
  std::vector<Dependency> deps;
  
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
      deps.push_back(dep);
    }
  }

  return deps;
}

void OpenMPTaskGraph::buildDependencyEdges() {
  // Build happens-before edges based on task dependencies
  
  for (size_t i = 0; i < m_tasks.size(); ++i) {
    Task *task_i = m_tasks[i].get();
    
    for (size_t j = i + 1; j < m_tasks.size(); ++j) {
      Task *task_j = m_tasks[j].get();
      if (task_i->parent_context != task_j->parent_context) {
        continue;
      }
      if (task_i->taskgroup_id != task_j->taskgroup_id) {
        continue;
      }
      
      // Check if tasks have conflicting dependencies
      for (const Dependency &dep_i : task_i->dependencies) {
        for (const Dependency &dep_j : task_j->dependencies) {
          if (dependenciesConflict(dep_i, dep_j)) {
            // Add edge: task_i must complete before task_j
            // (Program order determines ordering)
            task_i->successors.insert(task_j);
            task_j->predecessors.insert(task_i);
          }
        }
      }
    }
  }

  for (const auto &entry : m_wait_boundaries) {
    const Function *context = entry.first;
    for (size_t boundary : entry.second) {
      for (const auto &lhs : m_tasks) {
        if (lhs->parent_context != context || lhs->sequence_index >= boundary) {
          continue;
        }
        for (const auto &rhs : m_tasks) {
          if (rhs->parent_context != context || rhs->sequence_index <= boundary) {
            continue;
          }
          lhs->successors.insert(rhs.get());
          rhs->predecessors.insert(lhs.get());
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
  const Value *addr1 = stripValue(d1.address);
  const Value *addr2 = stripValue(d2.address);
  const Value *base1 = addr1 ? getUnderlyingObject(addr1) : nullptr;
  const Value *base2 = addr2 ? getUnderlyingObject(addr2) : nullptr;

  if (addr1 != addr2 && (!base1 || !base2 || stripValue(base1) != stripValue(base2))) {
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

  if (!addr1 || !addr2) {
    return false;
  }

  if (addr1 == addr2) {
    return true;
  }

  int64_t offset1 = 0;
  int64_t offset2 = 0;
  const Value *offset_base1 = GetPointerBaseWithConstantOffset(addr1, offset1, DL);
  const Value *offset_base2 = GetPointerBaseWithConstantOffset(addr2, offset2, DL);

  if (offset_base1 && offset_base2 &&
      stripValue(offset_base1) == stripValue(offset_base2) && d1.size != 0 &&
      d2.size != 0) {
    uint64_t begin1 = static_cast<uint64_t>(offset1);
    uint64_t begin2 = static_cast<uint64_t>(offset2);
    uint64_t end1 = begin1 + d1.size;
    uint64_t end2 = begin2 + d2.size;
    return begin1 < end2 && begin2 < end1;
  }

  return true;
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

bool OpenMPTaskGraph::mayBeParallel(const Task *t1, const Task *t2) const {
  return !happensBefore(t1, t2) && !happensBefore(t2, t1);
}
