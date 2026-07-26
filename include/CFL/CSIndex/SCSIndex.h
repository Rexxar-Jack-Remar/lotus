#pragma once

#include "CFL/CSIndex/Grail.h"
#include "CFL/CSIndex/PolicyAutomaton.h"
#include "CFL/CSIndex/SCSGraph.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

enum class ProductConstruction { Explicit, Lazy };

struct SCSBatchQuery {
  std::string name;
  std::vector<int> sources;
  std::vector<int> sinks;
};

struct SCSIndexOptions {
  ProductConstruction product_construction = ProductConstruction::Lazy;
  bool retain_witnesses = false;
  int grail_dimensions = 2;
};

struct SCSIndexStats {
  size_t explicit_product_states = 0;
  size_t materialized_product_states = 0;
  size_t product_vertices = 0;
  size_t product_edges = 0;
  size_t normalized_product_edges = 0;
  size_t summary_edges = 0;
  size_t indexing_vertices = 0;
  size_t indexing_edges = 0;
};

struct SCSWitness {
  std::vector<int> base_vertices;
  std::vector<int> base_edges;
  std::vector<int> structural_labels;
  std::vector<int> event_labels;
  bool context_valid = false;
  bool policy_accepting = false;
};

/**
 * Sanitizer-aware context-sensitive reachability index. Security events are
 * compiled into a product graph before the existing FLARE transformation.
 */
class SCSIndex {
public:
  SCSIndex(const SCSGraph &graph, const PolicyAutomaton &policy,
           std::vector<int> sources, std::vector<int> sinks,
           SCSIndexOptions options = {},
           std::vector<SCSBatchQuery> batches = {});
  ~SCSIndex();

  SCSIndex(const SCSIndex &) = delete;
  SCSIndex &operator=(const SCSIndex &) = delete;

  bool reachable(int source, int sink);
  bool reachableBatch(const std::string &batch_name);
  std::optional<SCSWitness> witness(int source, int sink) const;

  int startVertex(int source) const;
  int acceptVertex(int sink) const;
  int productVertex(int base_vertex, int policy_state) const;

  Graph &productGraph();
  Graph &indexingGraph();
  const SCSIndexStats &stats() const;

private:
  struct ProductVertexInfo {
    int base_vertex = -1;
    int policy_state = -1;
    bool intermediate = false;
  };

  std::pair<int, bool> getOrCreateProductVertex(int base_vertex,
                                                int policy_state);
  void addProductTransition(int source_product, int target_product,
                            const SCSEdge &base_edge);
  void addGraphEdge(Graph &graph, int source, int target, int label);
  void buildExplicitProduct();
  void buildLazyProduct();
  void buildIndex(const std::vector<SCSBatchQuery> &batches);
  void validateCatalog() const;

  std::vector<int> findOrdinaryPath(int source, int target) const;
  std::optional<std::vector<int>>
  expandProductPath(const std::vector<int> &ordinary_path) const;
  static void appendPath(std::vector<int> &path,
                         const std::vector<int> &suffix);
  bool contextValid(const std::vector<int> &structural_labels) const;
  bool policyAccepting(const std::vector<int> &event_labels) const;

  SCSGraph base_graph_;
  PolicyAutomaton policy_;
  std::vector<int> sources_;
  std::vector<int> sinks_;
  SCSIndexOptions options_;

  std::unique_ptr<Graph> product_graph_;
  std::unique_ptr<Graph> indexing_graph_;
  std::unique_ptr<Graph> dag_graph_;
  std::unique_ptr<Grail> grail_;

  std::vector<ProductVertexInfo> product_vertices_;
  std::map<std::pair<int, int>, int> product_vertex_map_;
  std::map<std::pair<int, int>, int> product_edge_origins_;
  std::map<int, int> start_vertices_;
  std::map<int, int> accept_vertices_;
  std::map<std::string, std::pair<int, int>> batch_vertices_;
  std::vector<int> scc_map_;

  int flare_vertex_count_ = 0;
  SCSIndexStats stats_;
};
