#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"
#include "CFL/Classical/Solvers/Engines/CERT/CertCFL.h"
#include <memory>
#include <vector>

namespace lotus::cfl::classical::engines {

struct CertCFLStatistics {
  cert::Statistics core;
  std::size_t logical_facts = 0; // All symbols, including seed facts and epsilon.
  std::size_t seed_facts = 0;    // Distinct buffered seed facts, not epsilon.
  // Growth of the completed snapshot, INCLUDING newly buffered input facts.
  // This follows the endpoint-quotient engine's snapshot-growth convention.
  std::size_t added_facts = 0;
  std::size_t duplicate_inputs = 0;
};

/// CERT-CFL adapter implementing Lotus's complete Relation interface.
///
/// add() buffers monotone seed insertions, including migrated nonterminal facts.
/// Queries see the last completed snapshot (empty before the first solve).
/// New nodes/edges become visible only after solve(). A failed solve preserves
/// the old snapshot and all buffered inputs; retrying is safe.
///
/// This version REBUILDS after updates; it does not implement delta reuse.
/// All grammar symbols are resolved because Relation permits arbitrary labels.
/// Use cert::solve directly for a selected-symbol observation contract.
/// Grammar rules are copied at construction, so no Grammar reference is kept.
/// Single-threaded mutation; do not update/solve from a traversal callback.
class CertCFLEngine final : public Relation {
public:
  CertCFLEngine(const Grammar &grammar, std::size_t node_count,
                cert::Options options = {});
  ~CertCFLEngine() override;
  CertCFLEngine(const CertCFLEngine &) = delete;
  CertCFLEngine &operator=(const CertCFLEngine &) = delete;
  void ensureNodeCount(std::size_t node_count) override;
  bool add(SymbolId symbol, NodeId source, NodeId target) override;
  CertCFLStatistics solve();
  const CertCFLStatistics &statistics() const;
  bool contains(SymbolId symbol, NodeId source, NodeId target) const override;
  bool visitSuccessors(SymbolId symbol, NodeId source, NodeVisitor visitor) const override;
  bool visitPredecessors(SymbolId symbol, NodeId target, NodeVisitor visitor) const override;
  bool visitEdges(EdgeVisitor visitor) const override;
  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const override;
  std::size_t edgeCount() const override;
  std::size_t edgeCount(SymbolId symbol) const override;
  std::size_t estimatedPayloadBytes() const override;
  std::size_t countOffDiagonalUnion(std::vector<SymbolId> symbols) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
