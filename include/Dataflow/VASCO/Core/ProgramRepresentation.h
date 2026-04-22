#pragma once

#include "Dataflow/VASCO/Core/DirectedGraph.h"

#include <memory>
#include <optional>
#include <vector>

namespace vasco {

template <typename M, typename N> class ProgramRepresentation {
public:
  using GraphPtr = std::shared_ptr<const DirectedGraph<N>>;

  virtual ~ProgramRepresentation() = default;

  virtual std::vector<M> getEntryPoints() const = 0;
  virtual GraphPtr getControlFlowGraph(const M &Method) const = 0;
  virtual bool isCall(const N &Node) const = 0;
  virtual std::optional<std::vector<M>>
  resolveTargets(const M &CallerMethod, const N &CallNode) const = 0;

  virtual bool isPhantomMethod(const M &) const { return false; }
};

} // namespace vasco
