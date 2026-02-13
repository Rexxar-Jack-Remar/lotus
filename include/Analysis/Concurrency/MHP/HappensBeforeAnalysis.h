#ifndef HAPPENS_BEFORE_ANALYSIS_H
#define HAPPENS_BEFORE_ANALYSIS_H

#include "Analysis/Concurrency/MHP/CppAtomics.h"
#include "Analysis/Concurrency/MHP/MHPAnalysis.h"
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <unordered_map>
#include <vector>

namespace lotus {

class AliasAnalysisWrapper;

class HappensBeforeAnalysis {
public:
  explicit HappensBeforeAnalysis(llvm::Module &module, mhp::MHPAnalysis &mhp);

  void analyze();

  /**
   * @brief Set optional alias analysis for synchronizes-with same-location check
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) { m_alias_analysis = aa; }

  /**
   * @brief Check if instruction A happens-before instruction B
   * Includes program order (TFG) and C11 synchronizes-with from atomics.
   * @param A The first instruction
   * @param B The second instruction
   * @return true if A happens-before B
   */
  bool happensBefore(const llvm::Instruction *A, const llvm::Instruction *B) const;

private:
  void buildSynchronizesWith();

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

  llvm::Module &m_module;
  mhp::MHPAnalysis &m_mhp;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;

  /// Pairs (release/seq_cst store, acquire/seq_cst load) on same location
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_sync_with;

  struct InstPairHash {
    size_t operator()(
        const std::pair<const llvm::Instruction *, const llvm::Instruction *>
            &p) const {
      return std::hash<const llvm::Instruction *>()(p.first) ^
             std::hash<const llvm::Instruction *>()(p.second);
    }
  };
  mutable std::unordered_map<
      std::pair<const llvm::Instruction *, const llvm::Instruction *>, bool,
      InstPairHash>
      m_hb_cache;
};

} // namespace lotus

#endif // HAPPENS_BEFORE_ANALYSIS_H

