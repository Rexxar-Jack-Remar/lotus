#pragma once

#include "Concurrency/CUDA/CUDAAbstractState.h"

namespace concurrency::cuda {

class CUDAKernelProtocolAnalysis {
public:
  explicit CUDAKernelProtocolAnalysis(const llvm::Function &kernel,
                                      CUDAAbstractState &state);

  void runAnalysis();

  const std::vector<CUDAProtocolEpoch> &getBarrierEpochs() const {
    return m_barrier_epochs;
  }

  const std::vector<CUDAProtocolEpoch> &getFenceEpochs() const {
    return m_fence_epochs;
  }

  bool hasProperSynchronization() const { return m_has_proper_sync; }

private:
  const llvm::Function &m_kernel;
  CUDAAbstractState &m_state;
  std::vector<CUDAProtocolEpoch> m_barrier_epochs;
  std::vector<CUDAProtocolEpoch> m_fence_epochs;
  bool m_has_proper_sync = true;
};

} // namespace concurrency::cuda