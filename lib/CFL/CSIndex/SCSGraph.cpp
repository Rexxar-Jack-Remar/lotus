#include "CFL/CSIndex/SCSGraph.h"

#include <stdexcept>

void SCSGraph::addVertex(int id, int func_id) {
  if (id < 0)
    throw std::invalid_argument("SCSGraph vertex IDs must be non-negative");

  const size_t old_size = vertices_.size();
  if (static_cast<size_t>(id) >= old_size) {
    vertices_.resize(id + 1);
    out_edges_.resize(id + 1);
    for (size_t index = old_size; index < vertices_.size(); ++index)
      vertices_[index].id = static_cast<int>(index);
  }
  vertices_[id].func_id = func_id;
}

int SCSGraph::addEdge(int source, int target, int structural_label,
                      int event_label) {
  if (!hasVertex(source) || !hasVertex(target))
    throw std::invalid_argument("SCSGraph edge endpoint does not exist");

  const int edge_id = static_cast<int>(edges_.size());
  edges_.push_back({edge_id, source, target, structural_label, event_label});
  out_edges_[source].push_back(edge_id);
  return edge_id;
}

int SCSGraph::addEventPredecessor(int vertex, int event_label) {
  if (!hasVertex(vertex))
    throw std::out_of_range("SCSGraph vertex ID is out of range");
  if (event_label == 0)
    throw std::invalid_argument("Synthetic event edges require an event label");

  const int predecessor = num_vertices();
  addVertex(predecessor, this->vertex(vertex).func_id);
  addEdge(predecessor, vertex, 0, event_label);
  return predecessor;
}

int SCSGraph::addEventSuccessor(int vertex, int event_label) {
  if (!hasVertex(vertex))
    throw std::out_of_range("SCSGraph vertex ID is out of range");
  if (event_label == 0)
    throw std::invalid_argument("Synthetic event edges require an event label");

  const int successor = num_vertices();
  addVertex(successor, this->vertex(vertex).func_id);
  addEdge(vertex, successor, 0, event_label);
  return successor;
}

bool SCSGraph::hasVertex(int id) const {
  return id >= 0 && static_cast<size_t>(id) < vertices_.size();
}

int SCSGraph::num_vertices() const {
  return static_cast<int>(vertices_.size());
}

int SCSGraph::num_edges() const { return static_cast<int>(edges_.size()); }

const SCSVertex &SCSGraph::vertex(int id) const {
  if (!hasVertex(id))
    throw std::out_of_range("SCSGraph vertex ID is out of range");
  return vertices_[id];
}

const SCSEdge &SCSGraph::edge(int edge_id) const {
  if (edge_id < 0 || static_cast<size_t>(edge_id) >= edges_.size())
    throw std::out_of_range("SCSGraph edge ID is out of range");
  return edges_[edge_id];
}

const std::vector<int> &SCSGraph::out_edges(int vertex_id) const {
  if (!hasVertex(vertex_id))
    throw std::out_of_range("SCSGraph vertex ID is out of range");
  return out_edges_[vertex_id];
}

const std::vector<SCSEdge> &SCSGraph::edges() const { return edges_; }

std::set<int> SCSGraph::observedEvents() const {
  std::set<int> events;
  for (const SCSEdge &edge : edges_) {
    if (edge.event_label != 0)
      events.insert(edge.event_label);
  }
  return events;
}

SCSGraph SCSGraph::fromGraph(Graph &graph, const EventLabelMap &event_labels) {
  SCSGraph result;
  for (int vertex = 0; vertex < graph.num_vertices(); ++vertex)
    result.addVertex(vertex, graph[vertex].func_id);

  for (int source = 0; source < graph.num_vertices(); ++source) {
    for (int target : graph.out_edges(source)) {
      int event_label = 0;
      const auto event_it = event_labels.find({source, target});
      if (event_it != event_labels.end())
        event_label = event_it->second;
      result.addEdge(source, target, graph.label(source, target), event_label);
    }
  }
  return result;
}
