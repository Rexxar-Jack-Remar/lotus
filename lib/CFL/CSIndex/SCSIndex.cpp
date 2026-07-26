#include "CFL/CSIndex/SCSIndex.h"

#include "CFL/CSIndex/GraphUtil.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <stdexcept>

SCSIndex::SCSIndex(const SCSGraph &graph, const PolicyAutomaton &policy,
                   std::vector<int> sources, std::vector<int> sinks,
                   SCSIndexOptions options, std::vector<SCSBatchQuery> batches)
    : base_graph_(graph), policy_(policy), sources_(std::move(sources)),
      sinks_(std::move(sinks)), options_(options),
      product_graph_(std::make_unique<Graph>()) {
  validateCatalog();

  std::string policy_error;
  if (!policy_.validate(base_graph_.observedEvents(), &policy_error))
    throw std::invalid_argument(policy_error);
  if (options_.grail_dimensions <= 0)
    throw std::invalid_argument("GRAIL dimensions must be positive");

  stats_.explicit_product_states =
      static_cast<size_t>(base_graph_.num_vertices()) * policy_.stateCount();
  if (options_.product_construction == ProductConstruction::Explicit)
    buildExplicitProduct();
  else
    buildLazyProduct();
  buildIndex(batches);
}

SCSIndex::~SCSIndex() = default;

void SCSIndex::validateCatalog() const {
  if (sources_.empty())
    throw std::invalid_argument("An SCS index requires at least one source");
  if (sinks_.empty())
    throw std::invalid_argument("An SCS index requires at least one sink");

  for (int source : sources_) {
    if (!base_graph_.hasVertex(source))
      throw std::invalid_argument("Source vertex is out of range");
  }
  for (int sink : sinks_) {
    if (!base_graph_.hasVertex(sink))
      throw std::invalid_argument("Sink vertex is out of range");
  }
}

std::pair<int, bool> SCSIndex::getOrCreateProductVertex(int base_vertex,
                                                        int policy_state) {
  const auto key = std::make_pair(base_vertex, policy_state);
  const auto existing = product_vertex_map_.find(key);
  if (existing != product_vertex_map_.end())
    return {existing->second, false};

  const int product_vertex = product_graph_->num_vertices();
  product_graph_->addVertex(product_vertex);
  product_graph_->at(product_vertex).func_id =
      base_graph_.vertex(base_vertex).func_id;
  product_vertex_map_[key] = product_vertex;
  product_vertices_.push_back({base_vertex, policy_state, false});
  return {product_vertex, true};
}

void SCSIndex::addGraphEdge(Graph &graph, int source, int target, int label) {
  if (label == 0)
    graph.addEdge(source, target);
  else
    graph.addEdge(source, target, label);
}

void SCSIndex::addProductTransition(int source_product, int target_product,
                                    const SCSEdge &base_edge) {
  const auto direct_key = std::make_pair(source_product, target_product);
  if (!product_graph_->hasEdge(source_product, target_product)) {
    addGraphEdge(*product_graph_, source_product, target_product,
                 base_edge.structural_label);
    product_edge_origins_[direct_key] = base_edge.id;
    return;
  }

  if (product_graph_->label(source_product, target_product) ==
      base_edge.structural_label) {
    // These product edges are semantically identical for SCS-LCR. Retain one
    // representative base edge for witness projection.
    return;
  }

  // Graph labels are keyed by endpoint pair. Preserve a colliding structural
  // role with the isolated degree-two normalization permitted by the model.
  const int intermediate = product_graph_->num_vertices();
  product_graph_->addVertex(intermediate);
  product_graph_->at(intermediate).func_id =
      base_graph_.vertex(base_edge.source).func_id;
  product_vertices_.push_back({-1, -1, true});

  product_graph_->addEdge(source_product, intermediate);
  addGraphEdge(*product_graph_, intermediate, target_product,
               base_edge.structural_label);
  product_edge_origins_[{source_product, intermediate}] = -1;
  product_edge_origins_[{intermediate, target_product}] = base_edge.id;
  ++stats_.normalized_product_edges;
}

void SCSIndex::buildExplicitProduct() {
  for (int vertex = 0; vertex < base_graph_.num_vertices(); ++vertex) {
    for (int state = 0; state < policy_.stateCount(); ++state)
      getOrCreateProductVertex(vertex, state);
  }

  for (const SCSEdge &edge : base_graph_.edges()) {
    for (int state = 0; state < policy_.stateCount(); ++state) {
      const int source_product = productVertex(edge.source, state);
      for (int successor : policy_.successors(state, edge.event_label)) {
        const int target_product = productVertex(edge.target, successor);
        addProductTransition(source_product, target_product, edge);
      }
    }
  }
}

void SCSIndex::buildLazyProduct() {
  std::deque<int> worklist;
  for (int source : sources_) {
    auto product = getOrCreateProductVertex(source, policy_.initialState());
    if (product.second)
      worklist.push_back(product.first);
  }

  while (!worklist.empty()) {
    const int source_product = worklist.front();
    worklist.pop_front();
    const ProductVertexInfo source_info = product_vertices_[source_product];
    assert(!source_info.intermediate);

    for (int edge_id : base_graph_.out_edges(source_info.base_vertex)) {
      const SCSEdge &edge = base_graph_.edge(edge_id);
      for (int successor :
           policy_.successors(source_info.policy_state, edge.event_label)) {
        auto target = getOrCreateProductVertex(edge.target, successor);
        addProductTransition(source_product, target.first, edge);
        if (target.second)
          worklist.push_back(target.first);
      }
    }
  }
}

void SCSIndex::buildIndex(const std::vector<SCSBatchQuery> &batches) {
  stats_.materialized_product_states = product_vertex_map_.size();
  stats_.product_vertices = product_graph_->num_vertices();
  stats_.product_edges = product_graph_->num_edges();

  indexing_graph_ = std::make_unique<Graph>(*product_graph_);
  indexing_graph_->build_summary_edges(options_.retain_witnesses);
  stats_.summary_edges = indexing_graph_->summary_edge_size();

  flare_vertex_count_ = indexing_graph_->num_vertices();
  indexing_graph_->to_indexing_graph();

  for (int source : sources_)
    start_vertices_[source] = productVertex(source, policy_.initialState());

  for (int sink : sinks_) {
    const int accept = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(accept);
    accept_vertices_[sink] = accept;

    for (int accepting_state : policy_.acceptingStates()) {
      const int product = productVertex(sink, accepting_state);
      if (product >= 0)
        indexing_graph_->addEdge(product + flare_vertex_count_, accept);
    }
  }

  for (const SCSBatchQuery &batch : batches) {
    if (batch.name.empty())
      throw std::invalid_argument("Batch query names cannot be empty");
    if (batch_vertices_.count(batch.name))
      throw std::invalid_argument("Duplicate batch query name");

    const int virtual_source = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(virtual_source);
    const int virtual_target = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(virtual_target);

    for (int source : batch.sources)
      indexing_graph_->addEdge(virtual_source, startVertex(source));
    for (int sink : batch.sinks)
      indexing_graph_->addEdge(acceptVertex(sink), virtual_target);
    batch_vertices_[batch.name] = {virtual_source, virtual_target};
  }

  stats_.indexing_vertices = indexing_graph_->num_vertices();
  stats_.indexing_edges = indexing_graph_->num_edges();

  dag_graph_ = std::make_unique<Graph>(*indexing_graph_);
  scc_map_.resize(indexing_graph_->num_vertices());
  std::vector<int> reverse_topological_order;
  GraphUtil::mergeSCC(*dag_graph_, scc_map_.data(), reverse_topological_order);
  GraphUtil::topo_leveler(*dag_graph_);
  grail_ = std::make_unique<Grail>(*dag_graph_, options_.grail_dimensions, 1,
                                   false, 100);
}

bool SCSIndex::reachable(int source, int sink) {
  return grail_->reach(scc_map_.at(startVertex(source)),
                       scc_map_.at(acceptVertex(sink)));
}

bool SCSIndex::reachableBatch(const std::string &batch_name) {
  const auto batch_it = batch_vertices_.find(batch_name);
  if (batch_it == batch_vertices_.end())
    throw std::out_of_range("Unknown SCS batch query");
  return grail_->reach(scc_map_.at(batch_it->second.first),
                       scc_map_.at(batch_it->second.second));
}

int SCSIndex::startVertex(int source) const {
  const auto source_it = start_vertices_.find(source);
  if (source_it == start_vertices_.end())
    throw std::out_of_range("Source is not in the indexed catalog");
  return source_it->second;
}

int SCSIndex::acceptVertex(int sink) const {
  const auto sink_it = accept_vertices_.find(sink);
  if (sink_it == accept_vertices_.end())
    throw std::out_of_range("Sink is not in the indexed catalog");
  return sink_it->second;
}

int SCSIndex::productVertex(int base_vertex, int policy_state) const {
  const auto product_it = product_vertex_map_.find({base_vertex, policy_state});
  if (product_it == product_vertex_map_.end())
    return -1;
  return product_it->second;
}

Graph &SCSIndex::productGraph() { return *product_graph_; }

Graph &SCSIndex::indexingGraph() { return *indexing_graph_; }

const SCSIndexStats &SCSIndex::stats() const { return stats_; }

std::vector<int> SCSIndex::findOrdinaryPath(int source, int target) const {
  std::vector<int> parent(indexing_graph_->num_vertices(), -1);
  std::deque<int> worklist;
  parent[source] = source;
  worklist.push_back(source);

  while (!worklist.empty() && parent[target] == -1) {
    const int vertex = worklist.front();
    worklist.pop_front();
    for (int successor : indexing_graph_->out_edges(vertex)) {
      if (parent[successor] != -1)
        continue;
      parent[successor] = vertex;
      worklist.push_back(successor);
    }
  }

  if (parent[target] == -1)
    return {};

  std::vector<int> path;
  for (int vertex = target;; vertex = parent[vertex]) {
    path.push_back(vertex);
    if (vertex == source)
      break;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

void SCSIndex::appendPath(std::vector<int> &path,
                          const std::vector<int> &suffix) {
  if (suffix.empty())
    return;
  size_t begin = 0;
  if (!path.empty() && path.back() == suffix.front())
    begin = 1;
  path.insert(path.end(), suffix.begin() + begin, suffix.end());
}

std::optional<std::vector<int>>
SCSIndex::expandProductPath(const std::vector<int> &ordinary_path) const {
  if (ordinary_path.empty())
    return std::nullopt;

  std::vector<int> product_path;
  product_path.push_back(ordinary_path.front());

  for (size_t index = 1; index < ordinary_path.size(); ++index) {
    const int source = ordinary_path[index - 1];
    const int target = ordinary_path[index];

    if (source >= 2 * flare_vertex_count_ ||
        target >= 2 * flare_vertex_count_) {
      continue;
    }
    if (source < flare_vertex_count_ &&
        target == source + flare_vertex_count_) {
      continue;
    }

    const bool first_layer =
        source < flare_vertex_count_ && target < flare_vertex_count_;
    const bool second_layer =
        source >= flare_vertex_count_ && target >= flare_vertex_count_;
    if (!first_layer && !second_layer)
      return std::nullopt;

    const int product_source = source % flare_vertex_count_;
    const int product_target = target % flare_vertex_count_;
    if (product_graph_->hasEdge(product_source, product_target)) {
      appendPath(product_path, {product_source, product_target});
      continue;
    }

    const std::vector<int> *summary =
        indexing_graph_->summary_witness(product_source, product_target);
    if (!summary)
      return std::nullopt;
    appendPath(product_path, *summary);
  }
  return product_path;
}

bool SCSIndex::contextValid(const std::vector<int> &structural_labels) const {
  std::vector<int> calls;
  for (int label : structural_labels) {
    if (label > 0) {
      calls.push_back(label);
    } else if (label < 0 && !calls.empty()) {
      if (calls.back() + label != 0)
        return false;
      calls.pop_back();
    }
  }
  return true;
}

bool SCSIndex::policyAccepting(const std::vector<int> &event_labels) const {
  std::set<int> states = {policy_.initialState()};
  for (int event : event_labels) {
    std::set<int> successors;
    for (int state : states) {
      const std::vector<int> next = policy_.successors(state, event);
      successors.insert(next.begin(), next.end());
    }
    states = std::move(successors);
    if (states.empty())
      return false;
  }

  for (int state : states) {
    if (policy_.isAccepting(state))
      return true;
  }
  return false;
}

std::optional<SCSWitness> SCSIndex::witness(int source, int sink) const {
  if (!options_.retain_witnesses)
    return std::nullopt;

  const std::vector<int> ordinary_path =
      findOrdinaryPath(startVertex(source), acceptVertex(sink));
  const auto product_path = expandProductPath(ordinary_path);
  if (!product_path)
    return std::nullopt;

  SCSWitness result;
  result.base_vertices.push_back(source);
  int current_vertex = source;
  for (size_t index = 1; index < product_path->size(); ++index) {
    const auto origin_it = product_edge_origins_.find(
        {product_path->at(index - 1), product_path->at(index)});
    if (origin_it == product_edge_origins_.end())
      return std::nullopt;
    if (origin_it->second < 0)
      continue;

    const SCSEdge &edge = base_graph_.edge(origin_it->second);
    if (edge.source != current_vertex)
      return std::nullopt;
    current_vertex = edge.target;
    result.base_edges.push_back(edge.id);
    result.base_vertices.push_back(edge.target);
    result.structural_labels.push_back(edge.structural_label);
    result.event_labels.push_back(edge.event_label);
  }

  if (current_vertex != sink)
    return std::nullopt;
  result.context_valid = contextValid(result.structural_labels);
  result.policy_accepting = policyAccepting(result.event_labels);
  if (!result.context_valid || !result.policy_accepting)
    return std::nullopt;
  return result;
}
