#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"

#include <type_traits>
#include <unordered_map>

namespace tpa {

// Points-to map template
//
// A PtsMap maps pointers to their points-to sets. This is used for:
// - Env: Maps top-level pointers (Pointer*) to points-to sets
// - Store: Maps memory objects (MemoryObject*) to points-to sets
//
// Template Parameter:
//   T: The key type (must be a pointer type: Pointer* or MemoryObject*)
//
// Update Strategies:
// - insert: Adds a single object to a points-to set
// - weakUpdate: Union with existing set (monotonic, never removes)
// - strongUpdate: Replaces existing set (used for definite assignments)
// - mergeWith: Merges another entire map into this one
//
// Design:
// - Used for incremental data flow analysis
// - Returns boolean indicating if the map changed (for worklist management)
template <typename T> class PtsMap {
private:
  static_assert(std::is_pointer<T>::value,
                "PtsMap only accept pointer as key type");

  // Internal mapping from key to points-to set
  using MapType = std::unordered_map<T, PtsSet>;
  MapType mapping;

public:
  using const_iterator = typename MapType::const_iterator;

  // Lookup the points-to set for a key
  // Returns empty set if key not present
  PtsSet lookup(T key) const {
    assert(key != nullptr);
    auto itr = mapping.find(key);
    if (itr == mapping.end())
      return PtsSet::getEmptySet();
    else
      return itr->second;
  }
  // Check if a key exists in the map
  bool contains(T key) const { return !lookup(key).empty(); }

  // Insert a single memory object into a key's points-to set
  // Creates the key with empty set if not present
  // Returns true if the set changed (for worklist management)
  bool insert(T key, const MemoryObject *obj) {
    assert(key != nullptr && obj != nullptr);

    auto itr = mapping.find(key);
    if (itr == mapping.end())
      itr = mapping.insert(std::make_pair(key, PtsSet::getEmptySet())).first;

    auto &set = itr->second;
    auto newSet = set.insert(obj);
    if (set == newSet)
      return false;
    else {
      set = newSet;
      return true;
    }
  }

  // Weak update: union with existing set
  // Used for points-to information that flows into a variable
  // Returns true if the set changed
  bool weakUpdate(T key, PtsSet pSet) {
    assert(key != nullptr);

    auto itr = mapping.find(key);
    if (itr == mapping.end()) {
      mapping.insert(std::make_pair(key, pSet));
      return true;
    } else {
      auto &set = itr->second;
      auto newSet = set.merge(pSet);
      if (newSet == set)
        return false;
      else {
        set = newSet;
        return true;
      }
    }
  }

  // Strong update: replace existing set
  // Used when a variable is definitely assigned (not additive)
  // Returns true if the set changed
  bool strongUpdate(T key, PtsSet pSet) {
    assert(key != nullptr);

    auto itr = mapping.find(key);
    if (itr == mapping.end()) {
      mapping.insert(std::make_pair(key, pSet));
      return true;
    } else {
      auto &set = itr->second;
      if (set == pSet)
        return false;
      else {
        set = pSet;
        return true;
      }
    }
  }

  // Merge another map into this one (weak updates)
  // Returns true if anything changed
  bool mergeWith(const PtsMap<T> &rhs) {
    bool ret = false;
    for (auto const &mapping : rhs)
      ret |= weakUpdate(mapping.first, mapping.second);
    return ret;
  }

  size_t size() const { return mapping.size(); }
  bool empty() const { return mapping.empty(); }
  const_iterator begin() const { return mapping.begin(); }
  const_iterator end() const { return mapping.end(); }
};

} // namespace tpa