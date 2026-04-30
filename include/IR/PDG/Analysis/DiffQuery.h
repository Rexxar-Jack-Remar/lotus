/**
 * @file DiffQuery.h
 * @brief Structural differencing queries for PDG scopes and query results.
 */

#pragma once

#include "IR/PDG/Analysis/QueryCore.h"

namespace pdg {

/// Structural diff result between two PDG subgraphs.
struct DiffQueryResult {
  std::vector<NodeDiffEntry> node_diffs;
  std::vector<EdgeDiffEntry> edge_diffs;
  DiffImpactSummary impact_summary;
  PDGQueryDiagnostics diagnostics;

  bool isIdentical() const;
};

/// Structural differencing for PDG query results or explicit scopes.
class DiffQuery {
public:
  explicit DiffQuery(
      ProgramGraph &pdg,
      NodeMatchStrategy strategy = NodeMatchStrategy::PointerIdentity)
      : pdg_(pdg), strategy_(strategy) {}

  DiffQueryResult diff(const PDGQueryResult &before, const PDGQueryResult &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

  DiffQueryResult diff(const PDGQueryScope &before, const PDGQueryScope &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
  NodeMatchStrategy strategy_;
};

} // namespace pdg
