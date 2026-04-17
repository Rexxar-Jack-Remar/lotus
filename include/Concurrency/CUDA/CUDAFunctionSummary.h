#pragma once

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace concurrency::cuda {

struct CUDAFunctionSummary {
  const llvm::Function *function = nullptr;
  std::vector<const llvm::Function *> callees;
  std::vector<const llvm::CallBase *> kernel_launches;
  std::vector<const llvm::CallBase *> memory_transfers;
  std::vector<const llvm::CallBase *> synchronizations;
  std::vector<const llvm::CallBase *> atomics;
  std::vector<const llvm::CallBase *> stream_ops;
  std::vector<const llvm::CallBase *> event_ops;
  std::vector<const llvm::CallBase *> texture_ops;
  std::vector<const llvm::CallBase *> surface_ops;
  bool is_device_function = false;
  bool is_host_wrapper = false;
  bool recursive = false;
  bool reaches_fixed_point = false;
};

class CUDAFunctionSummaryAnalysis {
public:
  explicit CUDAFunctionSummaryAnalysis(const llvm::Module &module);

  void runAnalysis();

  const std::map<const llvm::Function *, CUDAFunctionSummary> &
  getSummaries() const {
    return m_summaries;
  }

  const CUDAFunctionSummary *getSummary(const llvm::Function *fn) const;

private:
  const llvm::Module &m_module;
  std::map<const llvm::Function *, CUDAFunctionSummary> m_summaries;
  std::set<const llvm::Function *> m_visited;
};

} // namespace concurrency::cuda
