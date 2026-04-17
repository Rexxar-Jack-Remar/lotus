#include "Concurrency/CUDA/CUDAAbstractState.h"

namespace concurrency::cuda {

void CUDAAbstractState::clear() {
  kernel_fact_by_class.clear();
  transfer_fact_by_class.clear();
  synchronization_fact_by_class.clear();
  access_fact_by_class.clear();
  model_gap_by_class.clear();
  stream_automaton_by_class.clear();
  event_automaton_by_class.clear();
  protocol_epoch_by_class.clear();
  kernel_facts.clear();
  memory_transfer_facts.clear();
  synchronization_facts.clear();
  access_facts.clear();
  model_gaps.clear();
  stream_automata.clear();
  event_automata.clear();
  barrier_epochs.clear();
  fence_epochs.clear();
  function_summaries.clear();
  participant_sets.clear();
}

CUDAKernelFact &CUDAAbstractState::upsertKernelFact(size_t kernel_class_id) {
  return kernel_fact_by_class[kernel_class_id];
}

CUDAMemoryTransferFact &
CUDAAbstractState::upsertMemoryTransferFact(size_t transfer_class_id) {
  return transfer_fact_by_class[transfer_class_id];
}

CUDASynchronizationFact &
CUDAAbstractState::upsertSynchronizationFact(size_t synchronization_class_id) {
  return synchronization_fact_by_class[synchronization_class_id];
}

CUDAAccessFact &CUDAAbstractState::upsertAccessFact(size_t access_class_id) {
  return access_fact_by_class[access_class_id];
}

CUDAModelGap &CUDAAbstractState::upsertModelGap(size_t gap_class_id) {
  return model_gap_by_class[gap_class_id];
}

CUDAStreamAutomaton &
CUDAAbstractState::upsertStreamAutomaton(size_t stream_class_id) {
  return stream_automaton_by_class[stream_class_id];
}

CUDAEventAutomaton &
CUDAAbstractState::upsertEventAutomaton(size_t event_class_id) {
  return event_automaton_by_class[event_class_id];
}

CUDAProtocolEpoch &CUDAAbstractState::upsertProtocolEpoch(size_t epoch_id) {
  return protocol_epoch_by_class[epoch_id];
}

CUDAAbstractStateBuilder::CUDAAbstractStateBuilder(
    llvm::Module &module, const CUDAAbstractState &kernel_facts,
    const CUDAAbstractState &transfer_facts,
    const CUDAAbstractState &sync_facts, const CUDAAbstractState &access_facts)
    : m_module(module), m_kernel_facts(kernel_facts),
      m_transfer_facts(transfer_facts), m_sync_facts(sync_facts),
      m_access_facts(access_facts) {}

CUDAAbstractState CUDAAbstractStateBuilder::build() const {
  CUDAAbstractState state;

  for (const auto &pair : m_kernel_facts.kernel_fact_by_class) {
    state.kernel_facts.push_back(pair.second);
  }
  for (const auto &pair : m_transfer_facts.transfer_fact_by_class) {
    state.memory_transfer_facts.push_back(pair.second);
  }
  for (const auto &pair : m_sync_facts.synchronization_fact_by_class) {
    state.synchronization_facts.push_back(pair.second);
  }
  for (const auto &pair : m_access_facts.access_fact_by_class) {
    state.access_facts.push_back(pair.second);
  }
  for (const auto &pair : m_kernel_facts.stream_automaton_by_class) {
    state.stream_automata.push_back(pair.second);
  }
  for (const auto &pair : m_kernel_facts.event_automaton_by_class) {
    state.event_automata.push_back(pair.second);
  }

  return state;
}

} // namespace concurrency::cuda
