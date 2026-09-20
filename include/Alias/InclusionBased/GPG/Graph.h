#pragma once

#include "Alias/InclusionBased/GPG/GPU.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace llvm {
class BasicBlock;
class CallBase;
class Function;
} // namespace llvm

namespace lotus::gpg {

using GPBId = std::uint32_t;
using GPBIdSet = std::set<GPBId>;

enum class GPBKind : std::uint8_t {
  Normal,
  Start,
  End,
  Parameter,
  Return,
  DirectCall,
  IndirectCall,
  BottomCall,
};

struct GPB {
  GPBId id = 0;
  GPBKind kind = GPBKind::Normal;
  GPUSet gpus;
  GPUSet original_gpus;
  AccessSet may_definitions;
  const llvm::BasicBlock *origin_block = nullptr;
  const llvm::CallBase *callsite = nullptr;
  std::set<const llvm::Function *> callees;

  bool isStructural() const;
  bool empty() const { return gpus.empty(); }
};

struct ReachingState {
  std::map<GPBId, GPUSet> in;
  std::map<GPBId, GPUSet> out;
  std::map<GPBId, GPUSet> generated;
  std::map<GPBId, GPUSet> killed;
  std::map<GPBId, GPUSet> blocked;
  GPUSet queued;
};

struct ReachingPair {
  ReachingState without_blocking;
  ReachingState with_blocking;
};

struct GPGStats {
  std::size_t blocks = 0;
  std::size_t gpus = 0;
  std::size_t flow_edges = 0;
  std::size_t queued_gpus = 0;
};

class GPG;

struct CallExpansion {
  const GPG *callee = nullptr;
  GPUSet parameter_gpus;
  GPUSet return_gpus;
};

class GPG {
public:
  GPBId entry() const { return entry_; }
  GPBId exit() const { return exit_; }
  void setEntry(GPBId id) { entry_ = id; }
  void setExit(GPBId id) { exit_ = id; }

  const std::map<GPBId, GPB> &blocks() const { return blocks_; }
  std::map<GPBId, GPB> &blocks() { return blocks_; }
  const GPUSet &boundaryDefinitions() const { return boundary_definitions_; }
  void setBoundaryDefinitions(GPUSet definitions);
  const GPUSet &supportGPUs() const { return support_gpus_; }
  void setSupportGPUs(GPUSet gpus) { support_gpus_ = std::move(gpus); }

  bool hasBlock(GPBId id) const;
  GPB *getBlock(GPBId id);
  const GPB *getBlock(GPBId id) const;
  void addBlock(GPB block);
  void addEdge(GPBId from, GPBId to);
  void removeEdge(GPBId from, GPBId to);
  const GPBIdSet &predecessors(GPBId id) const;
  const GPBIdSet &successors(GPBId id) const;

  ReachingPair analyzeReaching(const TypeCompatibility &compatible,
                               unsigned k_limit) const;
  bool strengthReduce(const TypeCompatibility &compatible, unsigned k_limit,
                      bool use_blocking = true);
  bool applyStrengthReduction(const ReachingPair &reaching,
                              bool use_blocking = true);
  bool eliminateDeadGPUs(const TypeCompatibility &compatible, unsigned k_limit,
                         bool use_blocking = true,
                         const ReachingPair *precomputed = nullptr);
  bool eliminateEmptyGPBs();
  bool coalesce(const TypeCompatibility &compatible, unsigned k_limit);
  bool expandCall(GPBId call_block,
                  const std::vector<CallExpansion> &alternatives);
  bool replaceCallWithBottom(GPBId call_block);
  std::vector<GPBId> callBlocks() const;

  bool validate() const;
  bool isBottom() const;
  bool isIdentity() const;
  bool isReachable(GPBId from, GPBId to) const;
  std::vector<GPBId> reversePostOrder() const;
  GPGStats stats() const;

  static GPU makeBoundaryDefinition(const Access &source);

private:
  std::map<GPBId, GPB> blocks_;
  std::map<GPBId, GPBIdSet> predecessors_;
  std::map<GPBId, GPBIdSet> successors_;
  GPUSet boundary_definitions_;
  GPUSet support_gpus_;
  GPBId entry_ = 0;
  GPBId exit_ = 0;

  ReachingState computeReaching(bool use_blocking,
                                const ReachingState *without_blocking,
                                const TypeCompatibility &compatible,
                                unsigned k_limit) const;
  GPUSet reduceBlock(const GPB &block, const GPUProducerIndex &available,
                     const GPUProducerIndex &unblocked, unsigned k_limit,
                     GPUSet &queued) const;
  GPUSet computeKills(const GPB &block, const GPUSet &incoming,
                      const GPUSet &generated) const;
  GPUSet computeBlocked(const GPUSet &incoming, const GPUSet &generated,
                        const TypeCompatibility &compatible) const;
  bool preventsCoalescing(const GPU &later, const GPU &earlier,
                          const TypeCompatibility &compatible) const;
  void eraseBlock(GPBId id);
};

} // namespace lotus::gpg
