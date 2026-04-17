#pragma once

#include "Concurrency/CUDA/CUDAAnalysis.h"

#include <llvm/ADT/DenseMap.h>

namespace concurrency::cuda::detail {

struct AliasQueryResult {
  llvm::AliasResult relation = llvm::AliasResult::MayAlias;
  AliasPrecision precision = AliasPrecision::NonAffine;
  AliasSource source = AliasSource::Local;
};

struct LaunchOrderingState {
  struct StreamState {
    bool ordered_since_last_launch = false;
    SynchronizationScope scope = SynchronizationScope::None;
    LaunchOrderingSource source = LaunchOrderingSource::None;
    SynchronizationPrimitive primitive = SynchronizationPrimitive::None;
  };

  bool device_synchronized = false;
  llvm::DenseMap<const llvm::Value *, StreamState> stream_states;
  StreamState unknown_stream;
};

bool isNVVMKernel(const llvm::Function *function);
bool isCUDAKernelCandidate(const llvm::Function *function);
const llvm::Value *getPotentialStream(const llvm::CallBase *call);
LaunchOrderingSource getOrderingSource(ThreadAPI::TD_TYPE type);
SynchronizationScope getSyncScope(ThreadAPI::TD_TYPE type);
SynchronizationPrimitive getSynchronizationPrimitive(
    ThreadAPI::TD_TYPE type, const llvm::Instruction *inst = nullptr);
bool launchesOrdered(const std::vector<KernelLaunchInfo> &launches,
                     size_t earlier_idx, size_t later_idx);
AliasQueryResult queryAlias(const AccessInfo &lhs, const AccessInfo &rhs,
                            lotus::AliasAnalysisWrapper *aa);

} // namespace concurrency::cuda::detail
