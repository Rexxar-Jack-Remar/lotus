#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace lotus::cfl::dynamic_dyck {

using Vertex = std::int64_t;
using Label = unsigned;

enum class Parenthesis { Open, Close };

/// One bidirected edge pair. Closing edges are normalized to opening reverses.
struct Edge {
  Vertex source = 0;
  Vertex target = 0;
  Label label = 0;
  Parenthesis kind = Parenthesis::Open;

  Edge opening() const;
};

struct Graph {
  std::vector<Vertex> vertices;
  std::vector<Edge> edges;
};

enum class UpdateKind { Insert, Delete };

struct Update {
  UpdateKind kind = UpdateKind::Insert;
  Edge edge;
};

/// Read the artifact's numeric-node DOT subset (op--N/cp--N), optionally
/// wrapped in digraph { ... }. Isolated nodes and line comments are supported.
/// Unsupported labels and malformed records throw std::invalid_argument.
Graph parseDot(std::istream &input);

/// Read artifact records: A|D SOURCE TARGET op--N|cp--N.
std::vector<Update> parseUpdates(std::istream &input);

} // namespace lotus::cfl::dynamic_dyck
