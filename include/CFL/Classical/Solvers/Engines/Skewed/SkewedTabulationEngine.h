#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"
#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulation.h"

#include <cstddef>
#include <memory>

namespace lotus::cfl::classical::engines {

struct SkewedTabulationStatistics : skewed::Stats {
  /// Original-symbol facts added since the previous completed snapshot.
  std::size_t derived_facts = 0;
};

/// Lotus Relation adapter for the skewed-tabulation E/PE kernel.
///
/// The kernel is rebuilt after a changed input batch because propagating
/// edges are deliberately absent from the indexed join relation. This makes
/// late terminal additions exact without retaining an unsound incremental
/// state. Queries continue to see the previous completed snapshot until the
/// next successful solve. SolverSession filters unchanged calls.
class SkewedTabulationEngine final : public Relation {
public:
  SkewedTabulationEngine(const Grammar &grammar, std::size_t node_count);
  ~SkewedTabulationEngine() override;
  SkewedTabulationEngine(const SkewedTabulationEngine &) = delete;
  SkewedTabulationEngine &operator=(const SkewedTabulationEngine &) = delete;

  void ensureNodeCount(std::size_t node_count) override;
  bool add(SymbolId symbol, NodeId source, NodeId target) override;
  SkewedTabulationStatistics solve();
  const SkewedTabulationStatistics &statistics() const;

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override;
  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const override;
  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const override;
  bool visitEdges(EdgeVisitor visitor) const override;
  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const override;
  std::size_t edgeCount() const override;
  std::size_t edgeCount(SymbolId symbol) const override;
  std::size_t estimatedPayloadBytes() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
