#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Store.h"

#include <memory>
#include <unordered_map>

namespace tpa {

// Program-point input state cache for semi-sparse fixpoint iteration.
//
// Memo stores the incoming Store for each ProgramPoint. update() performs join
// semantics (mergeWith / weakUpdate), and callers use its boolean return value
// to decide whether a successor should be re-enqueued.
class Memo {
private:
  using MapType =
      std::unordered_map<ProgramPoint, std::shared_ptr<const Store>>;
  MapType inState;

public:
  Memo() = default;

  Memo(const Memo &) = delete;
  Memo(Memo &&) noexcept = default;
  Memo &operator=(const Memo &) = delete;
  Memo &operator=(Memo &&) = delete;

  // Return nullptr if no state has been recorded for pp.
  const Store *lookup(const ProgramPoint &pp) const {
    auto itr = inState.find(pp);
    if (itr == inState.end())
      return nullptr;
    else
      return itr->second.get();
  }

  // Retained snapshots are immutable. Changed input states receive a new
  // identity, so a candidate validates its entire input in constant time.
  std::shared_ptr<const Store> snapshot(const ProgramPoint &pp) const {
    auto itr = inState.find(pp);
    return itr == inState.end() ? nullptr : itr->second;
  }

  // Join incoming store into pp's cached in-state.
  // Returns true when the cached state strictly grows.
  template <typename StoreType>
  bool update(const ProgramPoint &pp, StoreType &&store) {
    static_assert(
        std::is_same<std::remove_cv_t<std::remove_reference_t<StoreType>>,
                     Store>::value,
        "Memo.update() only accept Store");
    auto itr = inState.find(pp);
    if (itr == inState.end()) {
      inState.emplace(pp,
                      std::make_shared<Store>(std::forward<StoreType>(store)));
      return true;
    } else {
      auto &current = itr->second;
      // Stores are allocated mutable; only Memo can mutate them, and only
      // when no immutable snapshot is retaining this version.
      if (current.use_count() == 1)
        return const_cast<Store *>(current.get())->mergeWith(store);
      // Avoid allocating a replacement for an unchanged join. Presence of
      // an explicitly empty binding still counts as a change.
      bool changed = false;
      for (const auto &binding : store)
        if (!current->hasBinding(binding.first) ||
            current->lookup(binding.first).merge(binding.second) !=
                current->lookup(binding.first)) {
          changed = true;
          break;
        }
      if (!changed)
        return false;
      auto replacement = std::make_shared<Store>(*current);
      replacement->mergeWith(store);
      current = std::move(replacement);
      return true;
    }
  }

  // Convenience update used by transfers that only write one abstract cell.
  // Creates the Store lazily when pp has not been seen.
  bool update(const ProgramPoint &pp, const MemoryObject *obj, PtsSet pSet) {
    auto itr = inState.find(pp);
    if (itr == inState.end()) {
      auto newStore = Store();
      newStore.strongUpdate(obj, pSet);
      inState.emplace(pp, std::make_shared<Store>(std::move(newStore)));
      return true;
    } else {
      auto &current = itr->second;
      if (current.use_count() == 1)
        return const_cast<Store *>(current.get())->weakUpdate(obj, pSet);
      if (current->hasBinding(obj) &&
          current->lookup(obj).merge(pSet) == current->lookup(obj))
        return false;
      auto replacement = std::make_shared<Store>(*current);
      replacement->weakUpdate(obj, pSet);
      current = std::move(replacement);
      return true;
    }
  }

  bool empty() const { return inState.empty(); }
  const MapType &entries() const { return inState; }
  void clear() { inState.clear(); }
};

} // namespace tpa
