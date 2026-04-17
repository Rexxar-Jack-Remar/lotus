#pragma once

#include "Concurrency/CUDA/CUDAAbstractState.h"

#include <map>
#include <set>
#include <vector>

namespace concurrency::cuda {

class CUDASteamAutomatonBuilder {
public:
  explicit CUDASteamAutomatonBuilder(CUDAAbstractState &state);

  void addStream(const llvm::Value *stream);
  void addEvent(const llvm::Instruction *record_inst,
                const llvm::Value *stream);
  void addEventWait(const llvm::Instruction *wait_inst,
                    const llvm::Value *event);
  void addStreamSync(const llvm::Instruction *sync_inst,
                     const llvm::Value *stream);
  void addDeviceSync(const llvm::Instruction *sync_inst);

  void finalize();

  const std::map<size_t, CUDAStreamAutomaton> &getAutomata() const {
    return m_stream_automata;
  }

private:
  CUDAAbstractState &m_state;
  std::map<size_t, CUDAStreamAutomaton> m_stream_automata;
  std::set<const llvm::Value *> m_seen_streams;
};

} // namespace concurrency::cuda