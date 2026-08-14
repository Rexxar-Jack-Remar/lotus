/**
 * @file LinuxKernelMemoryModel.h
 * @brief LKMM-oriented event extraction and race candidate analysis.
 */

#pragma once

#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace kernel {

class LinuxKernelExecutionGraph;
class LinuxKernelLockAnalysis;
class LinuxKernelProcessModel;

class LinuxKernelMemoryModel {
public:
  using EventID = std::size_t;

  enum class EventKind {
    PLAIN_READ,
    PLAIN_WRITE,
    MARKED_READ,
    MARKED_WRITE,
    ATOMIC_RMW,
    FENCE,
    LOCK_ACQUIRE,
    LOCK_RELEASE,
    RCU,
  };

  enum class MemoryOrder {
    NONE,
    RELAXED,
    ACQUIRE,
    RELEASE,
    ACQ_REL,
    FULL,
    COMPILER,
    UNKNOWN,
  };

  enum class RelationKind {
    PROGRAM_ORDER,
    DATA_DEPENDENCY,
    ADDRESS_DEPENDENCY,
    CONTROL_DEPENDENCY,
  };

  struct Event {
    EventID id = 0;
    const llvm::Instruction *inst = nullptr;
    const llvm::Value *object = nullptr;
    EventKind kind = EventKind::PLAIN_READ;
    MemoryOrder order = MemoryOrder::NONE;
    bool read = false;
    bool write = false;
    bool plain = false;
  };

  struct Relation {
    EventID from = 0;
    EventID to = 0;
    RelationKind kind = RelationKind::PROGRAM_ORDER;
  };

  LinuxKernelMemoryModel(const LinuxKernelProcessModel &process_model,
                         const LinuxKernelExecutionGraph &execution_graph,
                         const LinuxKernelLockAnalysis &lock_analysis)
      : process_model_(process_model), execution_graph_(execution_graph),
        lock_analysis_(lock_analysis) {}

  void analyze();

  const std::vector<Event> &getEvents() const { return events_; }
  const std::vector<Relation> &getRelations() const { return relations_; }

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findDataRaceCandidates() const;

private:
  const LinuxKernelProcessModel &process_model_;
  const LinuxKernelExecutionGraph &execution_graph_;
  const LinuxKernelLockAnalysis &lock_analysis_;

  std::vector<Event> events_;
  std::vector<Relation> relations_;
  std::map<const llvm::Instruction *, std::vector<EventID>> events_by_inst_;
  std::map<const llvm::Instruction *, std::vector<LockID>> protecting_locks_;

  void addEvent(const Event &event);
  void computeProtectingLocks();
  bool shareProtectingLock(const llvm::Instruction *lhs,
                           const llvm::Instruction *rhs) const;
};

} // namespace kernel
