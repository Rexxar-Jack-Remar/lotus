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
    bool usable_for_unknown_launch = false;
    SynchronizationScope scope = SynchronizationScope::None;
    LaunchOrderingSource source = LaunchOrderingSource::None;
    SynchronizationPrimitive primitive = SynchronizationPrimitive::None;
    const llvm::Value *stream = nullptr;
    const llvm::Instruction *boundary_inst = nullptr;
    HostStreamKind stream_kind = HostStreamKind::Unknown;
    llvm::SmallVector<size_t, 4> ordered_dependencies;
  };

  struct EventState {
    bool has_record = false;
    const llvm::Value *recorded_stream = nullptr;
    const llvm::Instruction *record_inst = nullptr;
    HostStreamKind recorded_stream_kind = HostStreamKind::Unknown;
    llvm::SmallVector<size_t, 4> recorded_dependencies;
  };

  bool device_synchronized = false;
  StreamState host_state;
  llvm::DenseMap<const llvm::Value *, StreamState> stream_states;
  StreamState default_stream;
  StreamState per_thread_default_stream;
  llvm::DenseMap<const llvm::Value *, EventState> event_states;
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
