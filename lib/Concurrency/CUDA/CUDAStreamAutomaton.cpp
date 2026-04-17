#include "Concurrency/CUDA/CUDAStreamAutomaton.h"

#include "Concurrency/CUDA/CUDASemantics.h"
#include "Concurrency/Utils/ThreadAPI.h"

namespace concurrency::cuda {

CUDASteamAutomatonBuilder::CUDASteamAutomatonBuilder(CUDAAbstractState &state)
    : m_state(state) {}

void CUDASteamAutomatonBuilder::addStream(const llvm::Value *stream) {
  if (!stream || m_seen_streams.count(stream)) {
    return;
  }
  m_seen_streams.insert(stream);

  size_t class_id = m_stream_automata.size();
  CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
  automaton.stream_class_id = class_id;
  automaton.stream = stream;
  automaton.current_state = StreamState::Created;
}

void CUDASteamAutomatonBuilder::addEvent(const llvm::Instruction *record_inst,
                                         const llvm::Value *stream) {
  if (!record_inst) {
    return;
  }

  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (!stream || pair.second.stream == stream) {
      CUDAStreamTransition transition;
      transition.inst = record_inst;
      transition.from_state = pair.second.current_state;
      transition.to_state = StreamState::Active;
      transition.is_ordering_boundary = false;
      pair.second.transitions.push_back(transition);
      pair.second.current_state = StreamState::Active;
      pair.second.pending_operations.push_back(record_inst);
    }
  }
}

void CUDASteamAutomatonBuilder::addEventWait(const llvm::Instruction *wait_inst,
                                             const llvm::Value *event) {
  (void)event;
  if (!wait_inst) {
    return;
  }

  for (auto &pair : m_stream_automata) {
    CUDAStreamTransition transition;
    transition.inst = wait_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Active;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.pending_operations.clear();
  }
}

void CUDASteamAutomatonBuilder::addStreamSync(
    const llvm::Instruction *sync_inst, const llvm::Value *stream) {
  if (!sync_inst) {
    return;
  }

  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (!stream || pair.second.stream == stream) {
      CUDAStreamTransition transition;
      transition.inst = sync_inst;
      transition.from_state = pair.second.current_state;
      transition.to_state = StreamState::Synchronized;
      transition.is_ordering_boundary = true;
      pair.second.transitions.push_back(transition);
      pair.second.current_state = StreamState::Synchronized;
      pair.second.is_ordered = true;
      pair.second.pending_operations.clear();
    }
  }
}

void CUDASteamAutomatonBuilder::addDeviceSync(
    const llvm::Instruction *sync_inst) {
  if (!sync_inst) {
    return;
  }

  for (auto &pair : m_stream_automata) {
    CUDAStreamTransition transition;
    transition.inst = sync_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Synchronized;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = StreamState::Synchronized;
    pair.second.is_ordered = true;
    pair.second.pending_operations.clear();
  }
}

void CUDASteamAutomatonBuilder::finalize() {
  for (auto &pair : m_stream_automata) {
    if (pair.second.current_state == StreamState::Created ||
        pair.second.current_state == StreamState::Active) {
      pair.second.is_exact = true;
    }
  }
}

} // namespace concurrency::cuda