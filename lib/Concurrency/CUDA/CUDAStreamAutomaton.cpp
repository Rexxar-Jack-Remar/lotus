#include "Concurrency/CUDA/CUDAStreamAutomaton.h"

#include "Concurrency/CUDA/CUDASemantics.h"
#include "Concurrency/Utils/ThreadAPI.h"

namespace concurrency::cuda {

namespace {

bool isNullStream(const llvm::Value *stream) {
  if (!stream) {
    return true;
  }
  if (const auto *cv = llvm::dyn_cast<llvm::Constant>(stream)) {
    return cv->isNullValue();
  }
  if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(stream)) {
    if (ce->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0))) {
        return ci->isZero();
      }
    }
  }
  return false;
}

} // anonymous namespace

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
  automaton.is_null_stream = isNullStream(stream);
}

void CUDASteamAutomatonBuilder::addEventObject(const llvm::Value *event) {
  if (!event || m_seen_events.count(event)) {
    return;
  }
  m_seen_events.insert(event);

  size_t class_id = m_event_automata.size();
  CUDAEventAutomaton &automaton = m_event_automata[class_id];
  automaton.event_class_id = class_id;
  automaton.event = event;
  automaton.current_state = EventState::Created;
}

void CUDASteamAutomatonBuilder::addEvent(const llvm::Instruction *record_inst,
                                         const llvm::Value *event,
                                         const llvm::Value *stream) {
  if (!record_inst) {
    return;
  }

  if (event) {
    addEventObject(event);
  }
  if (stream) {
    addStream(stream);
  } else {
    if (!m_seen_streams.count(nullptr)) {
      m_seen_streams.insert(nullptr);
      size_t class_id = m_stream_automata.size();
      CUDAStreamAutomaton &automaton = m_stream_automata[class_id];
      automaton.stream_class_id = class_id;
      automaton.stream = nullptr;
      automaton.current_state = StreamState::Created;
      automaton.is_null_stream = true;
    }
  }

  for (auto &pair : m_stream_automata) {
    if (!stream || pair.second.stream == stream || pair.second.is_null_stream) {
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

  for (auto &pair : m_event_automata) {
    if (event && pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = record_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Recorded;
    transition.is_ordering_boundary = false;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = EventState::Recorded;
    pair.second.recorded_stream = stream;
    pair.second.has_record = true;
  }
}

void CUDASteamAutomatonBuilder::addEventWait(const llvm::Instruction *wait_inst,
                                             const llvm::Value *event,
                                             const llvm::Value *stream) {
  if (!wait_inst) {
    return;
  }

  if (event) {
    addEventObject(event);
  }
  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (stream && pair.second.stream != stream && !pair.second.is_null_stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = wait_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Active;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.pending_operations.clear();
  }

  for (auto &pair : m_event_automata) {
    if (event && pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = wait_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Waited;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = EventState::Waited;
    pair.second.pending_waits.push_back(wait_inst);
    pair.second.has_wait = true;
  }
}

void CUDASteamAutomatonBuilder::addEventSync(const llvm::Instruction *sync_inst,
                                             const llvm::Value *event) {
  if (!sync_inst) {
    return;
  }
  if (event) {
    addEventObject(event);
  }

  for (auto &pair : m_event_automata) {
    if (event && pair.second.event != event) {
      continue;
    }
    CUDAEventTransition transition;
    transition.inst = sync_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = EventState::Synchronized;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = EventState::Synchronized;
    pair.second.pending_waits.clear();
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

void CUDASteamAutomatonBuilder::addStreamDestroy(
    const llvm::Instruction *destroy_inst, const llvm::Value *stream) {
  if (!destroy_inst) {
    return;
  }
  if (stream) {
    addStream(stream);
  }

  for (auto &pair : m_stream_automata) {
    if (stream && pair.second.stream != stream && !pair.second.is_null_stream) {
      continue;
    }
    CUDAStreamTransition transition;
    transition.inst = destroy_inst;
    transition.from_state = pair.second.current_state;
    transition.to_state = StreamState::Destroyed;
    transition.is_ordering_boundary = true;
    pair.second.transitions.push_back(transition);
    pair.second.current_state = StreamState::Destroyed;
    pair.second.is_ordered = true;
    pair.second.pending_operations.clear();
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
    m_state.stream_automata.push_back(pair.second);
    m_state.stream_automaton_by_class[pair.first] = pair.second;
  }

  for (auto &pair : m_event_automata) {
    if (pair.second.current_state == EventState::Created ||
        pair.second.current_state == EventState::Recorded ||
        pair.second.current_state == EventState::Waited ||
        pair.second.current_state == EventState::Synchronized) {
      pair.second.is_exact = true;
    }
    m_state.event_automata.push_back(pair.second);
    m_state.event_automaton_by_class[pair.first] = pair.second;
  }
}

} // namespace concurrency::cuda
