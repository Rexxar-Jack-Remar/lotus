#include "CFL/DynamicDyck/WeightedQuotient/Engine.h"
#include "CFL/DynamicDyck/WeightedQuotient/WeightedQuotientSolver.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace lotus::cfl::dynamic_dyck {

class WeightedQuotientSolver::Impl {
public:
  weighted_quotient::Engine engine;
  std::unique_ptr<weighted_quotient::CFLHashMap> original, merged;
  std::unordered_map<std::string, weighted_quotient::In_FastDLL<unsigned>>
      colors;
  std::unordered_map<unsigned, std::list<unsigned>> sets;
  std::unordered_map<Vertex, unsigned> node_ids;
  std::vector<Vertex> vertices;

  Impl()
      : original(std::make_unique<weighted_quotient::CFLHashMap>(0)),
        merged(std::make_unique<weighted_quotient::CFLHashMap>(0)) {
    engine.arrayreach(*merged, colors, sets);
  }

  bool addVertex(Vertex vertex) {
    if (node_ids.count(vertex))
      return false;
    if (vertices.size() >= std::numeric_limits<unsigned>::max())
      throw std::length_error("too many dynamic-Dyck vertices");
    const unsigned node = static_cast<unsigned>(vertices.size());
    node_ids.emplace(vertex, node);
    vertices.push_back(vertex);
    original->AddVertex();
    merged->AddVertex();
    engine.resp->arr.push_back(node);
    sets[node].push_back(node);
    return true;
  }
};

WeightedQuotientSolver::WeightedQuotientSolver()
    : m_impl(std::make_unique<Impl>()) {}

WeightedQuotientSolver::WeightedQuotientSolver(const Graph &graph)
    : WeightedQuotientSolver() {
  for (Vertex vertex : graph.vertices)
    m_impl->addVertex(vertex);
  for (Edge edge : graph.edges) {
    edge = edge.opening();
    m_impl->addVertex(edge.source);
    m_impl->addVertex(edge.target);
    m_impl->original->InsertEdge(m_impl->node_ids.at(edge.source),
                                 m_impl->node_ids.at(edge.target), edge.label);
  }
  m_impl->merged->CopyFrom(*m_impl->original);
  m_impl->engine.arrayreach(*m_impl->merged, m_impl->colors, m_impl->sets);
}

WeightedQuotientSolver::~WeightedQuotientSolver() = default;
WeightedQuotientSolver::WeightedQuotientSolver(
    WeightedQuotientSolver &&) noexcept = default;
WeightedQuotientSolver &
WeightedQuotientSolver::operator=(WeightedQuotientSolver &&) noexcept = default;

bool WeightedQuotientSolver::addVertex(Vertex vertex) {
  return m_impl->addVertex(vertex);
}

bool WeightedQuotientSolver::insertEdge(Edge edge) {
  edge = edge.opening();
  m_impl->addVertex(edge.source);
  m_impl->addVertex(edge.target);
  const unsigned source = m_impl->node_ids.at(edge.source),
                 target = m_impl->node_ids.at(edge.target);
  if (m_impl->original->HasEdgeBetween(source, target, edge.label))
    return false;
  m_impl->engine.insert(*m_impl->original, source, target, edge.label,
                        *m_impl->merged, m_impl->colors, m_impl->sets);
  return true;
}

bool WeightedQuotientSolver::deleteEdge(Edge edge) {
  edge = edge.opening();
  const auto source = m_impl->node_ids.find(edge.source),
             target = m_impl->node_ids.find(edge.target);
  if (source == m_impl->node_ids.end() || target == m_impl->node_ids.end() ||
      !m_impl->original->HasEdgeBetween(source->second, target->second,
                                        edge.label))
    return false;
  m_impl->engine.deleteE(*m_impl->original, source->second, target->second,
                         edge.label, *m_impl->merged, m_impl->colors,
                         m_impl->sets);
  return true;
}

bool WeightedQuotientSolver::apply(const Update &update) {
  switch (update.kind) {
  case UpdateKind::Insert:
    return insertEdge(update.edge);
  case UpdateKind::Delete:
    return deleteEdge(update.edge);
  }
  throw std::invalid_argument("invalid dynamic-Dyck update kind");
}

bool WeightedQuotientSolver::connected(Vertex source, Vertex target) const {
  const auto first = m_impl->node_ids.find(source),
             second = m_impl->node_ids.find(target);
  return first != m_impl->node_ids.end() && second != m_impl->node_ids.end() &&
         m_impl->engine.resp->find(first->second) ==
             m_impl->engine.resp->find(second->second);
}

Vertex WeightedQuotientSolver::representative(Vertex vertex) const {
  const auto node = m_impl->node_ids.find(vertex);
  if (node == m_impl->node_ids.end())
    throw std::out_of_range("unknown dynamic-Dyck vertex");
  return m_impl->vertices[m_impl->engine.resp->find(node->second)];
}

std::vector<std::vector<Vertex>> WeightedQuotientSolver::components() const {
  std::vector<std::vector<Vertex>> result;
  for (const auto &set : m_impl->sets) {
    if (set.second.empty())
      continue;
    result.emplace_back();
    for (unsigned node : set.second)
      result.back().push_back(m_impl->vertices[node]);
    std::sort(result.back().begin(), result.back().end());
  }
  std::sort(result.begin(), result.end());
  return result;
}

Graph WeightedQuotientSolver::graph() const {
  Graph result;
  result.vertices = m_impl->vertices;
  for (unsigned source = 0; source < result.vertices.size(); ++source) {
    std::unordered_map<unsigned, weighted_quotient::Matrix1> outgoing;
    m_impl->original->CheckOutEdges(source, outgoing);
    for (const auto &target : outgoing)
      for (const auto &color : target.second.colors)
        result.edges.push_back({result.vertices[source],
                                result.vertices[target.first], color.first});
  }
  return result;
}

Statistics WeightedQuotientSolver::statistics() const {
  Statistics result;
  result.vertices = m_impl->vertices.size();
  result.edges = m_impl->original->GetEdgNum();
  result.components = m_impl->sets.size();
  result.insertions = m_impl->engine.insertion_count;
  result.deletions = m_impl->engine.deletion_count;
  result.merges = m_impl->engine.merge_count;
  result.splits = m_impl->engine.split_count;
  result.cycle_rebuilds = m_impl->engine.cycle_rebuild_count;
  return result;
}

} // namespace lotus::cfl::dynamic_dyck
