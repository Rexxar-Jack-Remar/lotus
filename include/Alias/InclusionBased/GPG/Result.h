#pragma once

#include "Alias/InclusionBased/GPG/GPU.h"

#include <map>
#include <set>
#include <string>

namespace llvm {
class CallBase;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace lotus::gpg {

struct MemoryLocation;

struct AnalysisStats {
  std::size_t functions = 0;
  std::size_t initial_gpbs = 0;
  std::size_t initial_gpus = 0;
  std::size_t optimized_gpbs = 0;
  std::size_t optimized_gpus = 0;
  std::size_t indirect_calls = 0;
  std::size_t resolved_indirect_calls = 0;
  std::size_t call_graph_refinements = 0;
};

struct ModRefSummary {
  AccessSet modifications;
  AccessSet references;
};

/// A Value-level projection of GPG's Access-based points-to information.
///
/// `values` contains targets that have a concrete LLVM Value. `complete` is
/// false when target information remains unknown or cannot be represented by
/// the projection. Known null is tracked separately and does not itself make
/// the result incomplete; representing it as a Value would require an
/// analysis-external sentinel.
struct PointeeSetResult {
  std::set<const llvm::Value *> values;
  bool complete = false;
  bool contains_unknown = false;
  bool contains_null = false;

  bool isComplete() const { return complete; }
  bool containsUnknown() const { return contains_unknown; }
  bool containsNull() const { return contains_null; }
};

class GPGResult {
public:
  using ResolvedAccessMap = std::map<Access, AccessSet>;
  using PointsToMap = std::map<const llvm::Instruction *, ResolvedAccessMap>;
  using CallTargetMap =
      std::map<const llvm::CallBase *, std::set<const llvm::Function *>>;
  using ModRefMap = std::map<const llvm::Function *, ModRefSummary>;

  const PointsToMap &pointsToInformation() const { return points_to_; }
  const CallTargetMap &indirectCallTargets() const { return call_targets_; }
  const ModRefMap &modRefSummaries() const { return mod_ref_; }
  const AnalysisStats &stats() const { return stats_; }

  const AccessSet *resolvedAccesses(const llvm::Instruction *instruction,
                                    const Access &query) const;
  PointeeSetResult pointeeSet(const llvm::Instruction *instruction,
                              const llvm::Value *pointer) const;
  PointeeSetResult allPointeeSet(const llvm::Value *pointer) const;
  /// Compatibility projections that omit completeness/null/unknown metadata.
  /// Use pointeeSet()/allPointeeSet() before deriving strong alias answers.
  std::set<const llvm::Value *> pointees(const llvm::Instruction *instruction,
                                         const llvm::Value *pointer) const;
  std::set<const llvm::Value *> allPointees(const llvm::Value *pointer) const;
  const std::set<const llvm::Function *> *
  callTargets(const llvm::CallBase *call) const;

  void clear();
  void registerLocation(const MemoryLocation &location);
  void registerLocation(LocationId id, const llvm::Value *value,
                        std::string name);
  void registerValue(const llvm::Value *value, LocationId id);
  void recordGPU(const GPU &gpu);
  void recordCallTarget(const llvm::CallBase *call,
                        const llvm::Function *target);
  void setModRefSummary(const llvm::Function *function, ModRefSummary summary);
  void setStats(AnalysisStats stats) { stats_ = stats; }

private:
  PointsToMap points_to_;
  CallTargetMap call_targets_;
  ModRefMap mod_ref_;
  std::map<LocationId, const llvm::Value *> location_values_;
  std::map<LocationId, std::string> location_names_;
  std::map<const llvm::Value *, LocationId> value_locations_;
  std::set<LocationId> unknown_locations_;
  std::set<LocationId> null_locations_;
  AnalysisStats stats_;

  void addPointee(PointeeSetResult &result, const Access &target) const;
};

} // namespace lotus::gpg
