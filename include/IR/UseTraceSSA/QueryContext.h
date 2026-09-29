#pragma once

#include "IR/UseTraceSSA/TraceFlowGraph.h"
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallVector.h>

namespace lotus {
namespace usetracessa {
namespace detail {

// Preserve the complete flow-node ID; packing two IDs into a uint64_t would
// discard the upper half of a 64-bit node ID.
struct FlowStateKey {
  FlowNodeID node;
  ID state;
  bool operator==(const FlowStateKey &other) const {
    return node == other.node && state == other.state;
  }
};
struct FlowStateHash {
  std::size_t operator()(const FlowStateKey &key) const {
    return llvm::hash_combine(key.node, key.state);
  }
};

// Most bounded queries retain only a few calls. Keep these stacks inline and
// hash the full state, avoiding tree comparisons and temporary stack allocations.
struct ContextState {
  ID product;
  llvm::SmallVector<CallSiteID, 4> calls;
  bool truncated = false;
  bool positive = false;

  bool operator==(const ContextState &other) const {
    return product == other.product && truncated == other.truncated &&
           positive == other.positive && calls == other.calls;
  }
};

struct ContextStateHash {
  std::size_t operator()(const ContextState &state) const {
    return llvm::hash_combine(state.product, state.truncated, state.positive,
                             llvm::hash_combine_range(state.calls.begin(), state.calls.end()));
  }
};

} // namespace detail
} // namespace usetracessa
} // namespace lotus
