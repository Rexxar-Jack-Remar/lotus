#pragma once

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"

#include <unordered_map>

namespace tpa {

template <typename CallerType, typename CalleeType> class CallGraph {
private:
  using CalleeSet = util::VectorSet<CalleeType>;
  using CallerSet = util::VectorSet<CallerType>;
  using CalleeConstIterator = typename CalleeSet::const_iterator;
  using CallerConstIterator = typename CallerSet::const_iterator;

  using CalleeMap = std::unordered_map<CallerType, CalleeSet>;
  using CallerMap = std::unordered_map<CalleeType, CallerSet>;

  CalleeMap calleeMap;
  CallerMap callerMap;
  std::size_t revision = 0;
  const CallGraph *base = nullptr;
  std::vector<std::pair<CallerType, CalleeType>> addedEdges;

  bool hasEdge(const CallerType &caller, const CalleeType &callee) const {
    auto found = calleeMap.find(caller);
    if (found != calleeMap.end())
      return found->second.count(callee);
    return base && base->hasEdge(caller, callee);
  }

  template <typename MapType, typename KeyType, typename ValueType>
  static bool insertMap(MapType &m, const KeyType &k, const ValueType &v) {
    using SetType = util::VectorSet<ValueType>;

    auto mapInsertPair = m.insert(std::make_pair(k, SetType()));
    auto setInsertPair = mapInsertPair.first->second.insert(v);

    return mapInsertPair.second || setInsertPair.second;
  }

public:
  CallGraph() = default;

  // The base is immutable while a worker evaluates this view. Retirement
  // validates its captured revision before publishing only the added edges.
  CallGraph makeOverlay() const {
    CallGraph view;
    view.base = this;
    view.revision = revision;
    return view;
  }
  void commitOverlayTo(CallGraph &destination) const {
    assert(base);
    for (const auto &edge : addedEdges)
      destination.insertEdge(edge.first, edge.second);
  }

  bool insertEdge(const CallerType &caller, const CalleeType &callee) {
    if (base) {
      if (hasEdge(caller, callee))
        return false;
      // Copy just the two touched adjacency lists, once per view. Existing
      // edges and Return transfers require no allocation or graph copy.
      if (!calleeMap.count(caller)) {
        auto existing = base->getCallees(caller);
        calleeMap.emplace(caller, CalleeSet(existing.begin(), existing.end()));
      }
      if (!callerMap.count(callee)) {
        auto existing = base->getCallers(callee);
        callerMap.emplace(callee, CallerSet(existing.begin(), existing.end()));
      }
      insertMap(calleeMap, caller, callee);
      insertMap(callerMap, callee, caller);
      addedEdges.emplace_back(caller, callee);
      ++revision;
      return true;
    }
    auto ret0 = insertMap(calleeMap, caller, callee);
    auto ret1 = insertMap(callerMap, callee, caller);
    if (ret0 || ret1)
      ++revision;
    return ret0 || ret1;
  }
  std::size_t getRevision() const { return revision; }

  util::IteratorRange<CalleeConstIterator>
  getCallees(const CallerType &caller) const {
    auto itr = calleeMap.find(caller);
    if (itr == calleeMap.end() && base)
      return base->getCallees(caller);
    if (itr == calleeMap.end())
      return util::iteratorRange(CalleeConstIterator(), CalleeConstIterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }

  util::IteratorRange<CallerConstIterator>
  getCallers(const CalleeType &callee) const {
    auto itr = callerMap.find(callee);
    if (itr == callerMap.end() && base)
      return base->getCallers(callee);
    if (itr == callerMap.end())
      return util::iteratorRange(CallerConstIterator(), CallerConstIterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }
};

} // namespace tpa
