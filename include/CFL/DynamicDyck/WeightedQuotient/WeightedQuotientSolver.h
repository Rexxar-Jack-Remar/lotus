#pragma once

#include "CFL/DynamicDyck/Graph.h"
#include "CFL/DynamicDyck/Statistics.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace lotus::cfl::dynamic_dyck {

/// Exact dynamic reachability for one Dyck language on bidirected graphs.
///
/// Adapted from Li, Satya, and Zhang (POPL 2022), with cycle-safe deletion
/// motivated by Zhang's 2024 correction. This implementation does not claim
/// the original paper's update-time bound. Each operation completes saturation
/// synchronously. Separate instances own all their state; concurrent access
/// to the same instance requires synchronization because queries compress
/// paths.
class WeightedQuotientSolver {
public:
  WeightedQuotientSolver();
  explicit WeightedQuotientSolver(const Graph &graph);
  ~WeightedQuotientSolver();
  WeightedQuotientSolver(WeightedQuotientSolver &&) noexcept;
  WeightedQuotientSolver &operator=(WeightedQuotientSolver &&) noexcept;
  WeightedQuotientSolver(const WeightedQuotientSolver &) = delete;
  WeightedQuotientSolver &operator=(const WeightedQuotientSolver &) = delete;

  bool addVertex(Vertex vertex);

  // Insertion adds missing endpoints. Duplicate pairs (including an explicit
  // complementary reverse) are no-ops. Deletion removes the whole pair;
  // deleting an absent pair is a no-op and never creates vertices.
  bool insertEdge(Edge edge);
  bool deleteEdge(Edge edge);
  bool apply(const Update &update);

  // Reflexive on known vertices; unknown endpoints return false.
  bool connected(Vertex source, Vertex target) const;
  // Returns an original vertex ID; throws std::out_of_range for unknown IDs.
  // Representatives can change after either kind of update.
  Vertex representative(Vertex vertex) const;
  std::vector<std::vector<Vertex>> components() const;
  Graph graph() const;
  Statistics statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace lotus::cfl::dynamic_dyck
