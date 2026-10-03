// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace test {
using namespace lotus::cfl::dynamic_dyck;
inline PrimaryComponentConnectivityBackend test_backend =
    PrimaryComponentConnectivityBackend::Deterministic;
inline std::uint64_t checks = 0, semantic_states = 0, checked_pairs = 0;
inline void require(bool condition, const char *expression, const char *file,
                    int line) {
  ++checks;
  if (!condition)
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": " + expression);
}
#define CHECK(condition)                                                       \
  ::test::require(bool(condition), #condition, __FILE__, __LINE__)
template <class Exception, class F> void throws(F &&function) {
  bool caught = false;
  try {
    function();
  } catch (const Exception &) {
    caught = true;
  }
  CHECK(caught);
}
inline Edge close(Vertex u, Vertex v, Label a = 0) {
  return {u, v, a, Parenthesis::Close};
}
inline Edge open(Vertex u, Vertex v, Label a = 0) {
  return {u, v, a, Parenthesis::Open};
}
using Triple = std::tuple<Vertex, Vertex, Label>;
inline Triple key(Edge e) {
  if (e.kind == Parenthesis::Open)
    std::swap(e.source, e.target);
  return {e.source, e.target, e.label};
}

// Independent semantic specification: ordinary Boolean relation saturation of
// D -> epsilon | D D | open_a D close_a. No union-find, primary components,
// compressed summary edges or production implementation helpers are used.
struct Oracle {
  using Matrix = std::vector<std::vector<unsigned char>>;
  struct Relations {
    std::vector<Vertex> vertices;
    Matrix dyck, primary;
  };
  std::set<Vertex> vertices;
  std::map<Triple, std::uint64_t> counts;
  PrimaryComponentEdgeSemantics semantics;
  explicit Oracle(PrimaryComponentEdgeSemantics mode =
                      PrimaryComponentEdgeSemantics::ReferenceCounted)
      : semantics(mode) {}
  bool insert(Edge edge) {
    vertices.insert(edge.source);
    vertices.insert(edge.target);
    auto &count = counts[key(edge)];
    if (semantics == PrimaryComponentEdgeSemantics::Set && count)
      return false;
    ++count;
    return true;
  }
  bool erase(Edge edge) {
    const auto found = counts.find(key(edge));
    if (found == counts.end())
      return false;
    if (--found->second == 0)
      counts.erase(found);
    return true;
  }
  Relations relations() const {
    Relations result;
    result.vertices.assign(vertices.begin(), vertices.end());
    const auto n = result.vertices.size();
    std::map<Vertex, std::size_t> id;
    for (std::size_t i = 0; i < n; ++i)
      id[result.vertices[i]] = i;
    struct Arc {
      std::size_t from, to;
      Label label;
    };
    std::vector<Arc> closing, opening;
    for (const auto &entry : counts) {
      const auto &edge = entry.first;
      const auto u = id.at(std::get<0>(edge)), v = id.at(std::get<1>(edge));
      closing.push_back({u, v, std::get<2>(edge)});
      opening.push_back({v, u, std::get<2>(edge)});
    }
    result.dyck.assign(n, std::vector<unsigned char>(n));
    result.primary.assign(n, std::vector<unsigned char>(n));
    for (std::size_t i = 0; i < n; ++i)
      result.dyck[i][i] = result.primary[i][i] = 1;
    for (const Arc &left : opening)
      for (const Arc &right : closing)
        if (left.label == right.label && left.to == right.from)
          result.primary[left.from][right.to] = 1;
    for (std::size_t k = 0; k < n; ++k)
      for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
          if (result.primary[i][k] && result.primary[k][j])
            result.primary[i][j] = 1;
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t k = 0; k < n; ++k)
        for (std::size_t i = 0; i < n; ++i)
          for (std::size_t j = 0; j < n; ++j)
            if (result.dyck[i][k] && result.dyck[k][j] && !result.dyck[i][j]) {
              result.dyck[i][j] = 1;
              changed = true;
            }
      for (const Arc &left : opening)
        for (const Arc &right : closing)
          if (left.label == right.label && result.dyck[left.to][right.from] &&
              !result.dyck[left.from][right.to]) {
            result.dyck[left.from][right.to] = 1;
            changed = true;
          }
    }
    return result;
  }
};

inline void verify(PrimaryComponentSolver &solver, const Oracle &oracle,
                   bool structures = true) {
  ++semantic_states;
  const auto query_steps_before = solver.diagnostics().dscc_parent_steps;
  const auto expected = oracle.relations();
  std::vector<std::vector<Vertex>> components;
  std::set<Vertex> grouped;
  for (std::size_t i = 0; i < expected.vertices.size(); ++i) {
    const Vertex u = expected.vertices[i];
    const Vertex representative = solver.representative(u);
    const Vertex primary = solver.primaryRepresentative(u);
    const auto index = solver.vertexIndex(u);
    CHECK(solver.vertexAt(index) == u);
    CHECK(solver.representativeByIndex(index) == representative);
    CHECK(solver.primaryRepresentativeByIndex(index) == primary);
    CHECK(oracle.vertices.count(representative));
    CHECK(oracle.vertices.count(primary));
    std::vector<Vertex> component;
    for (std::size_t j = 0; j < expected.vertices.size(); ++j) {
      const Vertex v = expected.vertices[j];
      const bool reachable = bool(expected.dyck[i][j]);
      ++checked_pairs;
      if (solver.connected(u, v) != reachable) {
        std::ostringstream error;
        error << "Dyck mismatch " << u << " -> " << v << ", expected "
              << reachable;
        throw std::runtime_error(error.str());
      }
      CHECK((representative == solver.representative(v)) == reachable);
      CHECK(solver.connectedByIndex(index, solver.vertexIndex(v)) == reachable);
      CHECK((primary == solver.primaryRepresentative(v)) ==
            bool(expected.primary[i][j]));
      if (reachable)
        component.push_back(v);
    }
    if (!grouped.count(u)) {
      grouped.insert(component.begin(), component.end());
      components.push_back(std::move(component));
    }
  }
  CHECK(solver.components() == components);
  std::map<Triple, std::uint64_t> actual_counts;
  std::uint64_t references = 0;
  for (const auto &entry : solver.edgeCounts()) {
    CHECK(entry.edge.kind == Parenthesis::Open);
    actual_counts[key(entry.edge)] = entry.count;
    references += entry.count;
    CHECK(solver.edgeMultiplicity(entry.edge) == entry.count);
  }
  CHECK(actual_counts == oracle.counts);
  const auto stats = solver.statistics();
  CHECK(stats.vertices == oracle.vertices.size());
  CHECK(stats.edges == oracle.counts.size());
  CHECK(stats.components == components.size());
  CHECK(stats.cycle_rebuilds == 0);
  CHECK(solver.diagnostics().edge_references == references);
  if (solver.connectivityBackend() ==
      PrimaryComponentConnectivityBackend::Deterministic)
    CHECK(solver.diagnostics().dscc_parent_steps == query_steps_before);
  if (structures) {
    std::string error;
    if (!solver.validate(&error))
      throw std::runtime_error("structural invariant: " + error);
  }
}

struct Fixture {
  PrimaryComponentSolver solver;
  Oracle oracle;
  explicit Fixture(PrimaryComponentEdgeSemantics mode =
                       PrimaryComponentEdgeSemantics::ReferenceCounted)
      : solver(mode, test_backend), oracle(mode) {}
  void vertex(Vertex v) {
    CHECK(solver.addVertex(v) == oracle.vertices.insert(v).second);
  }
  void insert(Edge edge, bool check = true) {
    CHECK(solver.insertEdge(edge) == oracle.insert(edge));
    if (check)
      verify(solver, oracle);
  }
  void erase(Edge edge, bool check = true) {
    CHECK(solver.deleteEdge(edge) == oracle.erase(edge));
    if (check)
      verify(solver, oracle);
  }
  void expect(std::vector<std::vector<Vertex>> components) {
    for (auto &component : components)
      std::sort(component.begin(), component.end());
    std::sort(components.begin(), components.end());
    CHECK(solver.components() == components);
  }
};
} // namespace test
