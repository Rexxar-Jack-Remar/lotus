#pragma once

#include "Alias/InclusionBased/FlowSensitive/ValueFlowGraph.h"

#include <memory>
#include <set>
#include <vector>

namespace llvm {
class Function;
class Module;
class Value;
} // namespace llvm

namespace lotus::alias {

/// Context-insensitive, field-insensitive, flow-sensitive points-to analysis.
///
/// This analysis builds its own VFG from LLVM IR; it does not consume the
/// precomputed indirect edges of an SVFG or run an inclusion pre-analysis.
/// Aggregates and heap allocation sites are monolithic summary objects, as in
/// the paper. Null and unknown have explicit, distinct ObjectIDs.
///
/// The module must outlive the analysis, and must not be mutated between
/// analyze() and queries. analyze() reconstructs all state on every invocation.
class ValueFlowPTA {
public:
  using ObjectID = vfg::ObjectID;
  using PointsToSet = vfg::PointsToSet;
  using PointedToBySet = std::set<const llvm::Value *>;

  struct Config {
    bool enableStrongUpdates = true;
    // Empty: use a defined main; otherwise all externally visible definitions,
    // or all definitions if there are no externally visible ones.
    std::vector<const llvm::Function *> entryPoints;
  };
  struct Statistics {
    vfg::Solver::Statistics solver;
    std::size_t callGraphIterations = 0;
    std::size_t resolvedIndirectTargets = 0;
    std::size_t conservativeExternalCalls = 0;
    std::size_t objects = 0;
    std::size_t valueNodes = 0;
    std::size_t controlBlocks = 0;
  };

  explicit ValueFlowPTA(const llvm::Module &module);
  ValueFlowPTA(const llvm::Module &module, Config config);
  ~ValueFlowPTA();
  ValueFlowPTA(const ValueFlowPTA &) = delete;
  ValueFlowPTA &operator=(const ValueFlowPTA &) = delete;

  void analyze();
  // Empty for an untracked/non-pointer value; mayAlias remains conservative
  // for untracked values. Query results are invalidated by the next analyze().
  const PointsToSet &getPointsTo(const llvm::Value *value) const;
  const PointedToBySet &getPointedToBy(ObjectID object) const;
  bool mayAlias(const llvm::Value *left, const llvm::Value *right) const;
  ObjectID getObjectId(const llvm::Value *allocation) const;
  // Null for the two distinguished objects or an invalid ObjectID.
  const llvm::Value *getObjectValue(ObjectID object) const;
  ObjectID getNullObjectId() const;
  ObjectID getUnknownObjectId() const;
  const Statistics &getStatistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::alias
