/**
 * @file OpenMPTaskGraph.cpp
 * @brief Implementation of OpenMP Task Dependency Graph
 */

#include "Analysis/Concurrency/Utils/OpenMPTaskGraph.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace OpenMP;

OpenMPTaskGraph::OpenMPTaskGraph(Module &module) : m_module(module) {}

void OpenMPTaskGraph::analyze() {
  errs() << "Starting OpenMP Task Dependency Analysis...\n";
  
  identifyTasks();
  buildDependencyEdges();
  
  errs() << "Found " << m_tasks.size() << " OpenMP tasks with dependencies\n";
}

void OpenMPTaskGraph::identifyTasks() {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  
  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallInst *call = dyn_cast<CallInst>(&inst)) {
          // Check for __kmpc_omp_task_with_deps or __kmpc_omp_task
          ThreadAPI::TD_TYPE type = api->getType(api->getCallee(call));
          
          if (type == ThreadAPI::TD_OMP_TASK_WITH_DEPS || 
              type == ThreadAPI::TD_OMP_TASK) {
            auto task = std::make_unique<Task>();
            task->task_create = call;
            
            // Extract task function (typically the second argument)
            if (call->arg_size() >= 2) {
              if (Function *task_fn = dyn_cast<Function>(call->getArgOperand(1))) {
                task->task_function = task_fn;
              }
            }
            
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

std::vector<Dependency> OpenMPTaskGraph::extractDependencies(const CallInst *task_call) {
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
  if (const ConstantInt *CI = dyn_cast<ConstantInt>(ndeps_val)) {
    uint64_t ndeps = CI->getZExtValue();
    
    // Dependency list pointer
    const Value *dep_list = task_call->getArgOperand(4);
    
    // TODO: Parse the dependency list structure
    // This requires analyzing the memory layout of kmp_depend_info_t
    // For now, create placeholder dependencies
    for (uint64_t i = 0; i < ndeps; ++i) {
      Dependency dep;
      dep.type = DependType::INOUT; // Conservative
      dep.address = dep_list;        // Placeholder
      dep.size = 0;                  // Unknown
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
}

bool OpenMPTaskGraph::dependenciesConflict(const Dependency &d1, 
                                           const Dependency &d2) const {
  // Two dependencies conflict if:
  // 1. They access the same memory location (alias analysis needed)
  // 2. At least one is a write (OUT, INOUT, MUTEXINOUTSET)
  
  // TODO: Use alias analysis to check if addresses may alias
  // For now, conservative check: same address value
  if (d1.address != d2.address) {
    return false;
  }
  
  // Check for write dependency
  bool is_write1 = (d1.type == DependType::OUT || 
                    d1.type == DependType::INOUT ||
                    d1.type == DependType::MUTEXINOUTSET);
  bool is_write2 = (d2.type == DependType::OUT || 
                    d2.type == DependType::INOUT ||
                    d2.type == DependType::MUTEXINOUTSET);
  
  return is_write1 || is_write2;
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
