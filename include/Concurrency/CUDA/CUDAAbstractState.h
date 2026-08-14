#pragma once

#include "Concurrency/CUDA/CUDAFunctionSummary.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace concurrency::cuda {

enum class TransferKind {
  Unknown,
  HostToDevice,
  DeviceToHost,
  DeviceToDevice,
  HostToHost
};

enum class StreamState { Unknown, Created, Active, Synchronized, Destroyed };

enum class EventState {
  Unknown,
  Created,
  Recorded,
  Waited,
  Synchronized,
  Destroyed
};

enum class ProtocolState {
  Unknown,
  Initial,
  BarrierPending,
  BarrierActive,
  FenceRequired,
  WarpSyncRequired,
  Converged
};

enum class ParticipantCertainty { Unknown, Exact, Conditional, Partial };

struct CUDAStreamTransition {
  const llvm::Instruction *inst = nullptr;
  StreamState from_state = StreamState::Unknown;
  StreamState to_state = StreamState::Unknown;
  bool is_ordering_boundary = false;
};

struct CUDAEventTransition {
  const llvm::Instruction *inst = nullptr;
  EventState from_state = EventState::Unknown;
  EventState to_state = EventState::Unknown;
  bool is_ordering_boundary = false;
};

struct CUDAKernelFact {
  size_t kernel_class_id = 0;
  const llvm::Function *kernel = nullptr;
  const llvm::Instruction *launch_site = nullptr;
  const llvm::Value *stream = nullptr;
  bool stream_known = false;
  bool is_ordered_after_previous = false;
};

struct CUDAMemoryTransferFact {
  size_t transfer_class_id = 0;
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *src = nullptr;
  const llvm::Value *dst = nullptr;
  const llvm::Value *stream = nullptr;
  uint64_t size = 0;
  TransferKind kind = TransferKind::Unknown;
  bool is_async = false;
  bool stream_known = false;
};

struct CUDASynchronizationFact {
  size_t synchronization_class_id = 0;
  const llvm::Instruction *inst = nullptr;
  int primitive = 0;
  int scope = 0;
  bool ordering_effect = false;
  int participating_threads = 0;
};

struct CUDAAccessFact {
  size_t access_class_id = 0;
  const llvm::Instruction *instruction = nullptr;
  const llvm::Value *pointer = nullptr;
  const llvm::Value *base = nullptr;
  int space = 0;
  bool is_write = false;
  bool is_atomic = false;
  int uniformity = 0;
};

struct CUDAModelGap {
  size_t gap_class_id = 0;
  std::string explanation;
  double confidence = 0.0;
  std::vector<const llvm::Instruction *> related_instructions;
};

struct CUDAStreamAutomaton {
  size_t stream_class_id = 0;
  const llvm::Value *stream = nullptr;
  StreamState current_state = StreamState::Unknown;
  std::vector<CUDAStreamTransition> transitions;
  std::vector<const llvm::Instruction *> pending_operations;
  bool is_ordered = false;
  bool is_exact = false;
  bool is_null_stream = false;
};

struct CUDAEventAutomaton {
  size_t event_class_id = 0;
  const llvm::Value *event = nullptr;
  EventState current_state = EventState::Unknown;
  std::vector<CUDAEventTransition> transitions;
  std::vector<const llvm::Instruction *> pending_waits;
  const llvm::Value *recorded_stream = nullptr;
  bool has_record = false;
  bool has_wait = false;
  bool is_exact = false;
};

struct CUDAProtocolEpoch {
  size_t epoch_id = 0;
  const llvm::Function *kernel = nullptr;
  ProtocolState state = ProtocolState::Unknown;
  const llvm::Instruction *entry = nullptr;
  const llvm::Instruction *exit = nullptr;
  std::vector<const llvm::Instruction *> possible_exits;
  int scope = 0;
};

struct CUDAParticipantSet {
  const llvm::Function *kernel = nullptr;
  const llvm::Instruction *instruction = nullptr;
  std::vector<int> scopes;
  uint32_t min_lane = 0;
  uint32_t max_lane = 31;
  uint32_t min_warp = 0;
  uint32_t max_warp = 0;
  uint32_t min_block = 0;
  uint32_t max_block = 0;
  uint32_t min_grid = 0;
  uint32_t max_grid = 0;
  uint32_t lane_mask = 0xffffffffu;
  bool has_lane_mask = false;
  bool is_exact = false;
  bool is_symbolic = false;
  ParticipantCertainty certainty = ParticipantCertainty::Unknown;
};

class CUDAAbstractState {
public:
  std::map<size_t, CUDAKernelFact> kernel_fact_by_class;
  std::map<size_t, CUDAMemoryTransferFact> transfer_fact_by_class;
  std::map<size_t, CUDASynchronizationFact> synchronization_fact_by_class;
  std::map<size_t, CUDAAccessFact> access_fact_by_class;
  std::map<size_t, CUDAModelGap> model_gap_by_class;
  std::map<size_t, CUDAStreamAutomaton> stream_automaton_by_class;
  std::map<size_t, CUDAEventAutomaton> event_automaton_by_class;
  std::map<size_t, CUDAProtocolEpoch> protocol_epoch_by_class;
  std::map<const llvm::Function *, CUDAFunctionSummary> function_summaries;

  std::vector<CUDAKernelFact> kernel_facts;
  std::vector<CUDAMemoryTransferFact> memory_transfer_facts;
  std::vector<CUDASynchronizationFact> synchronization_facts;
  std::vector<CUDAAccessFact> access_facts;
  std::vector<CUDAModelGap> model_gaps;
  std::vector<CUDAStreamAutomaton> stream_automata;
  std::vector<CUDAEventAutomaton> event_automata;
  std::vector<CUDAProtocolEpoch> barrier_epochs;
  std::vector<CUDAProtocolEpoch> fence_epochs;
  std::vector<CUDAParticipantSet> participant_sets;

  void clear();
  CUDAKernelFact &upsertKernelFact(size_t kernel_class_id);
  CUDAMemoryTransferFact &upsertMemoryTransferFact(size_t transfer_class_id);
  CUDASynchronizationFact &
  upsertSynchronizationFact(size_t synchronization_class_id);
  CUDAAccessFact &upsertAccessFact(size_t access_class_id);
  CUDAModelGap &upsertModelGap(size_t gap_class_id);
  CUDAStreamAutomaton &upsertStreamAutomaton(size_t stream_class_id);
  CUDAEventAutomaton &upsertEventAutomaton(size_t event_class_id);
  CUDAProtocolEpoch &upsertProtocolEpoch(size_t epoch_id);

  const std::vector<CUDAKernelFact> &getKernelFacts() const {
    return kernel_facts;
  }
  const std::vector<CUDAMemoryTransferFact> &getMemoryTransferFacts() const {
    return memory_transfer_facts;
  }
  const std::vector<CUDASynchronizationFact> &getSynchronizationFacts() const {
    return synchronization_facts;
  }
  const std::vector<CUDAAccessFact> &getAccessFacts() const {
    return access_facts;
  }
  const std::vector<CUDAModelGap> &getModelGaps() const { return model_gaps; }
  const std::vector<CUDAStreamAutomaton> &getStreamAutomata() const {
    return stream_automata;
  }
  const std::vector<CUDAEventAutomaton> &getEventAutomata() const {
    return event_automata;
  }
  const std::vector<CUDAProtocolEpoch> &getBarrierEpochs() const {
    return barrier_epochs;
  }
  const std::vector<CUDAProtocolEpoch> &getFenceEpochs() const {
    return fence_epochs;
  }
  const std::vector<CUDAParticipantSet> &getParticipantSets() const {
    return participant_sets;
  }
  const std::map<const llvm::Function *, CUDAFunctionSummary> &
  getFunctionSummaries() const {
    return function_summaries;
  }
};

class CUDAAbstractStateBuilder {
public:
  CUDAAbstractStateBuilder(llvm::Module &module,
                           const CUDAAbstractState &kernel_facts,
                           const CUDAAbstractState &transfer_facts,
                           const CUDAAbstractState &sync_facts,
                           const CUDAAbstractState &access_facts);

  CUDAAbstractState build() const;

private:
  llvm::Module &m_module;
  const CUDAAbstractState &m_kernel_facts;
  const CUDAAbstractState &m_transfer_facts;
  const CUDAAbstractState &m_sync_facts;
  const CUDAAbstractState &m_access_facts;
};

} // namespace concurrency::cuda
