// Dense-hash points-to set using llvm::DenseSet.
//
// This file is standalone and not wired into any analysis yet.
#ifndef ANDERSEN_DENSE_HASH_PTSSET_H
#define ANDERSEN_DENSE_HASH_PTSSET_H

#include <cstddef>
#include <cstdint>

#include <llvm/ADT/DenseSet.h>

class DenseHashPtsSet {
public:
  using Index = std::uint64_t;
  using iterator = llvm::DenseSet<Index>::const_iterator;

  bool has(Index idx) {
    return static_cast<const DenseHashPtsSet &>(*this).has(idx);
  }

  bool has(Index idx) const { return set_.contains(idx); }

  bool insert(Index idx) { return set_.insert(idx).second; }

  bool contains(const DenseHashPtsSet &other) const {
    if (other.set_.size() > set_.size())
      return false;
    for (Index v : other.set_) {
      if (!set_.contains(v))
        return false;
    }
    return true;
  }

  bool intersectWith(const DenseHashPtsSet &other) const {
    if (set_.empty() || other.set_.empty())
      return false;
    const DenseHashPtsSet *smaller = this;
    const DenseHashPtsSet *larger = &other;
    if (other.set_.size() < set_.size()) {
      smaller = &other;
      larger = this;
    }
    for (Index v : smaller->set_) {
      if (larger->set_.contains(v))
        return true;
    }
    return false;
  }

  bool unionWith(const DenseHashPtsSet &other) {
    bool changed = false;
    for (Index v : other.set_)
      changed |= set_.insert(v).second;
    return changed;
  }

  void clear() { set_.clear(); }

  std::size_t getSize() const { return set_.size(); }

  bool isEmpty() const { return set_.empty(); }

  bool operator==(const DenseHashPtsSet &other) const {
    if (set_.size() != other.set_.size())
      return false;
    for (Index v : set_) {
      if (!other.set_.contains(v))
        return false;
    }
    return true;
  }

  iterator begin() const { return set_.begin(); }
  iterator end() const { return set_.end(); }

  bool intersects(const DenseHashPtsSet &other) const {
    return intersectWith(other);
  }

private:
  llvm::DenseSet<Index> set_;
};

#endif // ANDERSEN_DENSE_HASH_PTSSET_H
