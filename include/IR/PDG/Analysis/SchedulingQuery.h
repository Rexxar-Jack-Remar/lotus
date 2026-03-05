/**
 * @file SchedulingQuery.h
 * @brief Dependence-aware scheduling and parallelism queries over the PDG.
 */

#pragma once

#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include <set>
#include <unordered_map>
#include <vector>

namespace pdg {

struct SchedulingPolicy {
  /// Dependence edges considered as scheduling constraints. Empty = defaults.
  std::set<EdgeType> edge_types;
};

struct IndependenceResult {
  bool independent = false;

  std::vector<Node *> witness_path_ab;
  std::vector<EdgeType> witness_edge_types_ab;

  std::vector<Node *> witness_path_ba;
  std::vector<EdgeType> witness_edge_types_ba;
};

class SchedulingQuery {
public:
  using NodeSet = std::set<Node *>;

  explicit SchedulingQuery(GenericGraph &pdg) : _pdg(pdg) {}

  IndependenceResult independent(Node &a, Node &b,
                                 const SchedulingPolicy &policy = {});

  /**
   * @brief Compute ready-to-schedule nodes in @p region.
   *
   * A node is ready when all of its predecessor constraints inside @p region
   * have been satisfied by @p scheduled.
   */
  NodeSet readySet(const NodeSet &region, const NodeSet &scheduled,
                   const SchedulingPolicy &policy = {});

  /**
   * @brief Compute parallel scheduling wavefronts (Kahn levels).
   *
   * Returns topological levels for the induced dependence subgraph on @p
   * region. If cycles exist, cyclic SCC groups are emitted as final levels.
   */
  std::vector<NodeSet> topologicalLevels(const NodeSet &region,
                                         const SchedulingPolicy &policy = {});

  /**
   * @brief Return SCCs of the induced dependence subgraph.
   */
  std::vector<NodeSet>
  stronglyConnectedComponents(const NodeSet &region,
                              const SchedulingPolicy &policy = {});

  /**
   * @brief Estimate critical-path length in edge count.
   *
   * For acyclic regions this is exact. For cyclic regions, SCCs are collapsed
   * and weighted by SCC size to provide a conservative approximation.
   */
  size_t criticalPathLength(const NodeSet &region,
                            const SchedulingPolicy &policy = {});

private:
  GenericGraph &_pdg;

  static std::set<EdgeType> defaultSchedulingEdgeTypes();

  bool findPath(Node &source, Node &target,
                const std::set<EdgeType> &edge_types, std::vector<Node *> &path,
                std::vector<EdgeType> &path_edge_types) const;

  std::unordered_map<Node *, std::vector<Node *>>
  buildAdjacency(const NodeSet &region,
                 const std::set<EdgeType> &edge_types) const;
};

} // namespace pdg
