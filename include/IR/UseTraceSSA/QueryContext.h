#pragma once

#include "IR/UseTraceSSA/TraceFlowGraph.h"

#include <stdexcept>
#include <unordered_map>

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

// Intern call strings once per query. Ordinary CFG steps then copy/hash a
// stack ID, rather than copying the same call string at every product state.
class CallStrings {
  using Calls = llvm::SmallVector<CallSiteID, 4>;
  struct CallsHash {
    std::size_t operator()(const Calls &calls) const {
      return llvm::hash_combine_range(calls.begin(), calls.end());
    }
  };
  struct PushKey {
    ID stack;
    CallSiteID site;
    bool operator==(const PushKey &other) const {
      return stack == other.stack && site == other.site;
    }
  };
  struct PushHash {
    std::size_t operator()(const PushKey &key) const {
      return llvm::hash_combine(key.stack, key.site);
    }
  };
  struct Entry {
    Calls calls;
    ID parent = InvalidID;
  };
  std::vector<Entry> Entries;
  std::unordered_map<Calls, ID, CallsHash> Known;
  std::unordered_map<PushKey, ID, PushHash> Pushes;
  std::size_t Limit;
  ID intern(Calls calls) {
    auto found = Known.find(calls);
    if (found != Known.end())
      return found->second;
    if (Entries.size() >= InvalidID)
      throw std::length_error("UseTraceSSA: call-string identifier overflow");
    ID id = Entries.size();
    Known.emplace(calls, id);
    Entries.push_back({std::move(calls), InvalidID});
    return id;
  }

public:
  explicit CallStrings(std::size_t limit) : Limit(limit) { intern({}); }
  bool empty(ID stack) const { return stack == 0; }
  CallSiteID top(ID stack) const { return Entries[stack].calls.back(); }
  ID push(ID stack, CallSiteID site, bool &truncated) {
    truncated |= Entries[stack].calls.size() >= Limit;
    PushKey key{stack, site};
    auto cached = Pushes.find(key);
    if (cached != Pushes.end())
      return cached->second;
    Calls calls = Entries[stack].calls;
    if (calls.size() >= Limit && !calls.empty())
      calls.erase(calls.begin());
    calls.push_back(site);
    ID id = intern(std::move(calls));
    Pushes.emplace(key, id);
    return id;
  }
  ID pop(ID stack) {
    if (Entries[stack].parent != InvalidID)
      return Entries[stack].parent;
    Calls calls = Entries[stack].calls;
    calls.pop_back();
    ID parent = intern(std::move(calls));
    Entries[stack].parent = parent;
    return parent;
  }
};

struct InternedContextState {
  ID product, stack = 0;
  bool truncated = false, positive = false;
  bool operator==(const InternedContextState &other) const {
    return product == other.product && stack == other.stack &&
           truncated == other.truncated && positive == other.positive;
  }
};
struct InternedContextHash {
  std::size_t operator()(const InternedContextState &state) const {
    return llvm::hash_combine(state.product, state.stack, state.truncated,
                              state.positive);
  }
};

} // namespace detail
} // namespace usetracessa
} // namespace lotus
