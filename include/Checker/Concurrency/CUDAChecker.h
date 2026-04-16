#pragma once

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/CUDA/CUDAAnalysis.h"

#include <llvm/IR/Module.h>

#include <memory>
#include <vector>

namespace concurrency {

class CUDAChecker {
public:
  CUDAChecker(llvm::Module &module, cuda::CUDAAnalysis *analysis = nullptr);

  std::vector<ConcurrencyBugReport> checkCUDABugs();

private:
  llvm::Module &m_module;
  cuda::CUDAAnalysis *m_analysis;
  std::unique_ptr<cuda::CUDAAnalysis> m_owned_analysis;
};

} // namespace concurrency
