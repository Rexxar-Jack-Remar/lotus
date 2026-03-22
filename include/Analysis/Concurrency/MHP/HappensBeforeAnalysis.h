#ifndef HAPPENS_BEFORE_ANALYSIS_H
#define HAPPENS_BEFORE_ANALYSIS_H

#include "Analysis/Concurrency/MHP/MHPAnalysis.h"
#include "Analysis/Concurrency/Utils/CppAtomics.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace lotus {

class AliasAnalysisWrapper;

/**
 * Happens-before relation for race detection. HB is the union of:
 * - Program order (TFG): intra-thread and fork/join/lock/barrier edges.
 * - Synchronizes-with (m_sync_with): promise/future and selected modeled
 *   library/runtime edges.
 *
 * Atomic release/acquire edges are emitted only for narrowly witnessed,
 * single-partner cases. Broader atomic and fence patterns remain deferred until
 * the analysis has a defensible reads-from / release-sequence model.
 */
class HappensBeforeAnalysis {
public:
  explicit HappensBeforeAnalysis(llvm::Module &module, mhp::MHPAnalysis &mhp);

  void analyze();

  /**
   * @brief Set optional alias analysis for synchronizes-with same-location
   * check
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_alias_analysis = aa;
  }

  const std::unordered_map<std::string, size_t> &getDeferredSyncCounts() const {
    return m_deferred_sync_counts;
  }
  const std::vector<std::pair<const llvm::Instruction *,
                              const llvm::Instruction *>> &
  getSynchronizesWithEdges() const {
    return m_sync_with;
  }

  /**
   * @brief Check if instruction A happens-before instruction B
   * Includes program order (TFG) and modeled synchronizes-with edges.
   * @param A The first instruction
   * @param B The second instruction
   * @return true if A happens-before B
   */
  bool happensBefore(const llvm::Instruction *A,
                     const llvm::Instruction *B) const;

  bool mustPrecede(const llvm::Instruction *A,
                   const llvm::Instruction *B) const {
    return happensBefore(A, B);
  }

private:
  struct InstPairHash {
    size_t operator()(const std::pair<const llvm::Instruction *,
                                      const llvm::Instruction *> &p) const {
      return std::hash<const llvm::Instruction *>()(p.first) ^
             std::hash<const llvm::Instruction *>()(p.second);
    }
  };

  struct AtomicSyncWitness {
    const llvm::Instruction *release = nullptr;
    const llvm::Instruction *acquire = nullptr;
    const llvm::Value *location = nullptr;
  };

  void buildSynchronizesWith();
  void computeAtomicHappensBefore();
  std::vector<const llvm::Instruction *>
  collectFenceWitnesses(const llvm::Instruction *fence,
                        bool require_release_semantics) const;
  bool hasConcreteFenceWitness(const llvm::Instruction *release_inst,
                               const llvm::Instruction *acquire_inst) const;
  bool hasConcreteDirectAtomicWitness(const llvm::Instruction *release_inst,
                                      const llvm::Instruction *acquire_inst) const;
  bool hasConcreteReleaseSequenceWitness(
      const llvm::Instruction *release_inst,
      const llvm::Instruction *acquire_inst) const;
  bool atomicLocationsMustAlias(const llvm::Instruction *lhs,
                                const llvm::Instruction *rhs) const;
  size_t countConcreteAtomicWitnesses(const llvm::Instruction *inst) const;
  size_t countDirectAtomicPartners(const llvm::Instruction *inst,
                                   bool require_release_partner) const;
  bool atomicLocationsMayAlias(const llvm::Instruction *lhs,
                               const llvm::Instruction *rhs) const;
  bool hasProgramOrder(const llvm::Instruction *A,
                       const llvm::Instruction *B) const;
  bool canReach(const mhp::SyncNode *start, const mhp::SyncNode *end) const;
  bool canReachWithHB(const mhp::SyncNode *start,
                      const mhp::SyncNode *end) const;
  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;
  void addExtraHBEdge(const llvm::Instruction *from,
                      const llvm::Instruction *to);
  void addExplicitHBClosure(const llvm::Instruction *from,
                            const llvm::Instruction *to);
  void addExplicitHBPair(const llvm::Instruction *from,
                         const llvm::Instruction *to);
  std::vector<const llvm::Instruction *>
  collectThreadPrefixInstructions(const llvm::Instruction *inst) const;
  std::vector<const llvm::Instruction *>
  collectThreadSuffixInstructions(const llvm::Instruction *inst) const;

  bool sameAtomicLocation(const llvm::Instruction *store_inst,
                          const llvm::Instruction *load_inst) const;

  /**
   * @brief Check if promise and future operate on the same shared state
   */
  bool samePromiseFuturePair(const llvm::Instruction *promise,
                             const llvm::Instruction *future) const;

  /**
   * @brief Check if two call_once calls use the same once_flag
   */
  bool sameOnceFlag(const llvm::Instruction *call1,
                    const llvm::Instruction *call2) const;

  /**
   * @brief Check if two latch operations use the same latch object
   */
  bool sameLatch(const llvm::Instruction *inst1,
                 const llvm::Instruction *inst2) const;

  /**
   * @brief Check if two barrier operations use the same barrier object
   */
  bool sameBarrier(const llvm::Instruction *inst1,
                   const llvm::Instruction *inst2) const;

  const llvm::Value *traceSharedState(const llvm::Value *value) const;

  llvm::Module &m_module;
  mhp::MHPAnalysis &m_mhp;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_future_shared_state;
  mutable std::unordered_map<const llvm::Value *, const llvm::Value *>
      m_shared_state_trace_cache;
  std::unordered_map<std::string, size_t> m_deferred_sync_counts;
  std::vector<const llvm::Instruction *> m_atomic_instructions;
  std::vector<AtomicSyncWitness> m_atomic_sync_witnesses;
  std::unordered_set<std::pair<const llvm::Instruction *,
                               const llvm::Instruction *>, InstPairHash>
      m_atomic_hb_pairs;
  std::unordered_map<const mhp::SyncNode *, std::vector<const mhp::SyncNode *>>
      m_extra_hb_successors;

  /// Synchronizes-with pairs proven by non-atomic witness mechanisms.
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_sync_with;
  std::unordered_set<std::pair<const llvm::Instruction *,
                               const llvm::Instruction *>, InstPairHash>
      m_explicit_hb_pairs;
  mutable std::unordered_map<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, bool,
      InstPairHash>
      m_hb_cache;
};

} // namespace lotus

#endif // HAPPENS_BEFORE_ANALYSIS_H
