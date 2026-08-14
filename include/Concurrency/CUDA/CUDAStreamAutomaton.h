#pragma once

#include "Concurrency/CUDA/CUDAAbstractState.h"

#include <map>
#include <set>
#include <vector>

namespace concurrency::cuda {

class CUDASteamAutomatonBuilder {
public:
  explicit CUDASteamAutomatonBuilder(CUDAAbstractState &state);

  void addStreamCreate(const llvm::Instruction *create_inst,
                       const llvm::Value *output_slot);
  void addStream(const llvm::Value *stream);
  void addStreamOperation(const llvm::Instruction *operation_inst,
                          const llvm::Value *stream);
  void addEventCreate(const llvm::Instruction *create_inst,
                      const llvm::Value *output_slot);
  void addEvent(const llvm::Instruction *record_inst, const llvm::Value *event,
                const llvm::Value *stream);
  void addEventWait(const llvm::Instruction *wait_inst,
                    const llvm::Value *event, const llvm::Value *stream);
  void addEventSync(const llvm::Instruction *sync_inst,
                    const llvm::Value *event);
  void addEventDestroy(const llvm::Instruction *destroy_inst,
                       const llvm::Value *event);
  void addStreamSync(const llvm::Instruction *sync_inst,
                     const llvm::Value *stream);
  void addStreamDestroy(const llvm::Instruction *destroy_inst,
                        const llvm::Value *stream);
  void addDeviceSync(const llvm::Instruction *sync_inst);

  void finalize();

  const std::map<size_t, CUDAStreamAutomaton> &getAutomata() const {
    return m_stream_automata;
  }
  const std::map<size_t, CUDAEventAutomaton> &getEventAutomata() const {
    return m_event_automata;
  }

private:
  const llvm::Value *canonicalizeStream(const llvm::Value *stream) const;
  const llvm::Value *canonicalizeEvent(const llvm::Value *event) const;
  void recordOutputAliases(
      const llvm::Value *output_slot, const llvm::Value *identity,
      std::map<const llvm::Value *, const llvm::Value *> &aliases);
  void addEventObject(const llvm::Value *event);

  CUDAAbstractState &m_state;
  std::map<size_t, CUDAStreamAutomaton> m_stream_automata;
  std::map<size_t, CUDAEventAutomaton> m_event_automata;
  std::set<const llvm::Value *> m_seen_streams;
  std::set<const llvm::Value *> m_seen_events;
  std::map<const llvm::Value *, const llvm::Value *> m_stream_aliases;
  std::map<const llvm::Value *, const llvm::Value *> m_event_aliases;
};

} // namespace concurrency::cuda
