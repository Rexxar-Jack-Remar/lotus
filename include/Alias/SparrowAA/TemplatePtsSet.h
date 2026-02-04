#ifndef ANDERSEN_TEMPLATE_PTSSET_H
#define ANDERSEN_TEMPLATE_PTSSET_H

#include "Alias/SparrowAA/PtsSet.h"
#include "Alias/PtsSet/BDDPtsSet.h"

#include <memory>

// An enumeration for the available points-to set implementations
enum class PtsSetImpl { SPARSE_BITVECTOR, BDD };

// Runtime-selectable points-to set that keeps the public interface of
// the previous SparseBitVector-backed class while allowing a BDD backend.
class RuntimePtsSet {
public:
  using Index = std::uint64_t;
  using iterator = std::vector<Index>::const_iterator;

  RuntimePtsSet();
  RuntimePtsSet(const RuntimePtsSet &);
  RuntimePtsSet(RuntimePtsSet &&) noexcept = default;
  RuntimePtsSet &operator=(const RuntimePtsSet &);
  RuntimePtsSet &operator=(RuntimePtsSet &&) noexcept = default;
  ~RuntimePtsSet() = default;

  bool has(Index idx);
  bool has(Index idx) const;
  bool insert(Index idx);
  bool contains(const RuntimePtsSet &other) const;
  bool intersectWith(const RuntimePtsSet &other) const;
  bool unionWith(const RuntimePtsSet &other);

  void clear();
  unsigned getSize() const;
  bool isEmpty() const;
  bool operator==(const RuntimePtsSet &other) const;

  iterator begin() const;
  iterator end() const;

  static void selectImplementation(PtsSetImpl impl);
  static PtsSetImpl selectedImplementation();

private:
  struct Concept {
    virtual ~Concept() = default;
    virtual bool has(Index) const = 0;
    virtual bool insert(Index) = 0;
    virtual bool contains(const Concept &) const = 0;
    virtual bool intersectWith(const Concept &) const = 0;
    virtual bool unionWith(const Concept &) = 0;
    virtual void clear() = 0;
    virtual unsigned getSize() const = 0;
    virtual bool isEmpty() const = 0;
    virtual bool equals(const Concept &) const = 0;
    virtual std::unique_ptr<Concept> clone() const = 0;
    virtual void materialize(std::vector<Index> &out) const = 0;
  };

  template <typename Impl> struct Model : Concept {
    Impl set;

    bool has(Index idx) const override { return set.has(static_cast<unsigned>(idx)); }
    bool insert(Index idx) override { return set.insert(static_cast<unsigned>(idx)); }

    bool contains(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.contains(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      for (Index v : tmp)
        if (!set.has(static_cast<unsigned>(v)))
          return false;
      return true;
    }

    bool intersectWith(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.intersectWith(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      for (Index v : tmp)
        if (set.has(static_cast<unsigned>(v)))
          return true;
      return false;
    }

    bool unionWith(const Concept &other) override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.unionWith(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      bool changed = false;
      for (Index v : tmp)
        changed |= set.insert(static_cast<unsigned>(v));
      return changed;
    }

    void clear() override { set.clear(); }
    unsigned getSize() const override { return set.getSize(); }
    bool isEmpty() const override { return set.isEmpty(); }

    bool equals(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set == same->set;
      std::vector<Index> lhs;
      std::vector<Index> rhs;
      materialize(lhs);
      other.materialize(rhs);
      return lhs == rhs;
    }

    std::unique_ptr<Concept> clone() const override {
      return std::make_unique<Model>(*this);
    }

    void materialize(std::vector<Index> &out) const override {
      for (auto it = set.begin(), ie = set.end(); it != ie; ++it)
        out.push_back(static_cast<Index>(*it));
    }
  };

  static std::unique_ptr<Concept> makeImpl();
  void refreshCache() const;

  std::unique_ptr<Concept> impl;
  mutable std::shared_ptr<std::vector<Index>> cache;

  static PtsSetImpl &activeImpl();
};

inline void selectGlobalPtsSetImpl(PtsSetImpl impl) {
  RuntimePtsSet::selectImplementation(impl);
}

inline PtsSetImpl getGlobalPtsSetImpl() {
  return RuntimePtsSet::selectedImplementation();
}

// Preserve the previous name used across Andersen implementation.
using DefaultPtsSet = RuntimePtsSet;

// === Inline implementation details ===================================== //

inline std::unique_ptr<RuntimePtsSet::Concept> RuntimePtsSet::makeImpl() {
  if (activeImpl() == PtsSetImpl::BDD)
    return std::make_unique<Model<BDDAndersPtsSet>>();
  return std::make_unique<Model<AndersPtsSet>>();
}

inline RuntimePtsSet::RuntimePtsSet() : impl(makeImpl()) {}

inline RuntimePtsSet::RuntimePtsSet(const RuntimePtsSet &other)
    : impl(other.impl->clone()), cache(other.cache) {}

inline RuntimePtsSet &
RuntimePtsSet::operator=(const RuntimePtsSet &other) {
  if (this == &other)
    return *this;
  impl = other.impl->clone();
  cache = other.cache;
  return *this;
}

inline void RuntimePtsSet::selectImplementation(PtsSetImpl impl) {
  activeImpl() = impl;
}

inline PtsSetImpl RuntimePtsSet::selectedImplementation() {
  return activeImpl();
}

inline bool RuntimePtsSet::has(Index idx) {
  return static_cast<const RuntimePtsSet &>(*this).has(idx);
}

inline bool RuntimePtsSet::has(Index idx) const { return impl->has(idx); }

inline bool RuntimePtsSet::insert(Index idx) {
  cache.reset();
  return impl->insert(idx);
}

inline bool RuntimePtsSet::contains(const RuntimePtsSet &other) const {
  return impl->contains(*other.impl);
}

inline bool RuntimePtsSet::intersectWith(const RuntimePtsSet &other) const {
  return impl->intersectWith(*other.impl);
}

inline bool RuntimePtsSet::unionWith(const RuntimePtsSet &other) {
  cache.reset();
  return impl->unionWith(*other.impl);
}

inline void RuntimePtsSet::clear() {
  cache.reset();
  impl->clear();
}

inline unsigned RuntimePtsSet::getSize() const { return impl->getSize(); }

inline bool RuntimePtsSet::isEmpty() const { return impl->isEmpty(); }

inline bool RuntimePtsSet::operator==(const RuntimePtsSet &other) const {
  return impl->equals(*other.impl);
}

inline void RuntimePtsSet::refreshCache() const {
  if (cache)
    return;
  auto elems = std::make_shared<std::vector<Index>>();
  impl->materialize(*elems);
  cache = elems;
}

inline RuntimePtsSet::iterator RuntimePtsSet::begin() const {
  refreshCache();
  return cache->begin();
}

inline RuntimePtsSet::iterator RuntimePtsSet::end() const {
  refreshCache();
  return cache->end();
}

inline PtsSetImpl &RuntimePtsSet::activeImpl() {
  static PtsSetImpl impl = PtsSetImpl::SPARSE_BITVECTOR;
  return impl;
}

#endif // ANDERSEN_TEMPLATE_PTSSET_H 