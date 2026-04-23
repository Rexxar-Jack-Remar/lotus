#pragma once

#include "Solvers/EGraph/EGraph.h"

namespace lotus::egraph {

template <typename L, typename A>
inline std::string toDot(const EGraph<L, A> &egraph) {
  std::ostringstream oss;
  oss << "digraph egraph {\n";
  for (Id id : egraph.classIds()) {
    const auto &klass = egraph[id];
    oss << "  class_" << id.value() << " [label=\"EClass " << id.value() << "\"];\n";
    for (size_t i = 0; i < klass.nodes.size(); ++i) {
      oss << "  node_" << id.value() << "_" << i << " [label=\""
          << displayNode(klass.nodes[i]) << "\"];\n";
      oss << "  class_" << id.value() << " -> node_" << id.value() << "_" << i
          << ";\n";
      for (Id child : klass.nodes[i].children()) {
        oss << "  node_" << id.value() << "_" << i << " -> class_"
            << egraph.find(child).value() << ";\n";
      }
    }
  }
  oss << "}\n";
  return oss.str();
}

} // namespace lotus::egraph
