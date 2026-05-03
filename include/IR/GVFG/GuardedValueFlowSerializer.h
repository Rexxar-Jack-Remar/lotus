/// @file GuardedValueFlowSerializer.h
/// @brief Serialization of GuardedValueFlowGraph to text and Graphviz DOT formats

#pragma once

#include <string>

namespace lotus {
namespace gvfg {

class GuardedValueFlowGraph;

/// Static utility for writing a `GuardedValueFlowGraph` to human-readable
/// text or DOT for debugging.
class GuardedValueFlowSerializer {
public:
  /// Return a compact text description of the graph.
  static std::string toText(const GuardedValueFlowGraph &graph);
  /// Return a Graphviz DOT representation of the graph.
  static std::string toDot(const GuardedValueFlowGraph &graph);

  /// Write the text description to a file.
  static bool writeText(const GuardedValueFlowGraph &graph,
                        const std::string &filename);
  /// Write the DOT representation to a file.
  static bool writeDot(const GuardedValueFlowGraph &graph,
                       const std::string &filename);
};

} // namespace gvfg
} // namespace lotus
