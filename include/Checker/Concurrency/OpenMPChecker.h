#pragma once

#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <memory>
#include <vector>

namespace concurrency {

class OpenMPChecker {
public:
  OpenMPChecker(llvm::Module &module, OpenMP::OpenMPTaskGraph *task_graph,
                ThreadAPI *thread_api);

  std::vector<ConcurrencyBugReport> checkOpenMPBugs();

private:
  llvm::Module &m_module;
  OpenMP::OpenMPTaskGraph *m_taskGraph;
  std::unique_ptr<OpenMP::OpenMPTaskGraph> m_ownedTaskGraph;
  ThreadAPI *m_threadAPI;

  void ensureTaskGraph();
  std::vector<ConcurrencyBugReport> checkPartialTaskSynchronization() const;
  std::vector<ConcurrencyBugReport> checkTaskgroupStructure() const;
  std::vector<ConcurrencyBugReport> checkAtomicRegionStructure() const;
};

} // namespace concurrency