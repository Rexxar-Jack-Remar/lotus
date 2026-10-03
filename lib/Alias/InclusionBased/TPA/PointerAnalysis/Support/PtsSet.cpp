#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"

namespace tpa {

std::array<PtsSet::Shard, 32> PtsSet::existingSets;
const PtsSet::SetType *PtsSet::emptySet = uniquifySet(SetType{});

const PtsSet::SetType *PtsSet::uniquifySet(SetType &&set) {
  if (set.count(MemoryManager::getUniversalObject()))
    set = {MemoryManager::getUniversalObject()};

  auto &shard =
      existingSets[util::ContainerHasher<SetType>{}(set) % existingSets.size()];
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto &existingSet = shard.sets;

  auto itr = existingSet.find(set);
  if (itr == existingSet.end()) {
    set.shrink_to_fit();
    itr = existingSet.insert(itr, std::move(set));
  }

  return &*itr;
}

PtsSet PtsSet::insert(const MemoryObject *obj) const {
  if (pSet->count(obj))
    return *this;

  SetType newSet(*pSet);
  newSet.insert(obj);

  return PtsSet(uniquifySet(std::move(newSet)));
}

PtsSet PtsSet::merge(const PtsSet &rhs) const {
  // The easy case
  if (pSet == rhs.pSet)
    return *this;
  if (pSet == emptySet)
    return rhs;
  if (rhs.pSet == emptySet)
    return *this;
  // Sets containing Universal normalize to this singleton. Preserve the
  // lattice join without allocating a temporary vector or taking an intern
  // lock.
  const auto *universal = MemoryManager::getUniversalObject();
  if (pSet->size() == 1 && pSet->front() == universal)
    return *this;
  if (rhs.pSet->size() == 1 && rhs.pSet->front() == universal)
    return rhs;

  SetType newSet(*pSet);
  newSet.merge(*rhs.pSet);
  return PtsSet(uniquifySet(std::move(newSet)));
}

PtsSet PtsSet::getEmptySet() { return PtsSet(emptySet); }

PtsSet PtsSet::getSingletonSet(const MemoryObject *obj) {
  SetType newSet = {obj};
  return PtsSet(uniquifySet(std::move(newSet)));
}

std::vector<const MemoryObject *> PtsSet::intersects(const PtsSet &s0,
                                                     const PtsSet &s1) {
  return SetType::intersects(*s0.pSet, *s1.pSet);
}

PtsSet PtsSet::mergeAll(const std::vector<PtsSet> &sets) {
  size_t totSize = 0;
  for (auto const &pSet : sets)
    totSize += pSet.size();

  SetType flatSet;
  flatSet.reserve(totSize);

  for (auto const &pSet : sets)
    flatSet.merge(*pSet.pSet);

  return uniquifySet(SetType(std::move(flatSet)));
}

} // namespace tpa
