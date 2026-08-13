/*
 *
 * Author: rainoftime
 */
#include "Concurrency/Utils/FBVClock.h"

#include <stdexcept>
#include <utility>
#include <vector>

std::vector<FBVClock::ClockSystem> FBVClock::sys;

FBVClock::ClockSystemID FBVClock::new_clock_system() {
  for (std::size_t slot = 0; slot < sys.size(); ++slot) {
    ClockSystem &system = sys[slot];
    if (system.allocated)
      continue;
    if (system.generation == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("FBVClock system generation exhausted");
    ++system.generation;
    system.allocated = true;
    return ClockSystemID(slot, system.generation);
  }

  sys.emplace_back();
  ClockSystem &system = sys.back();
  system.allocated = true;
  system.generation = 1;
  return ClockSystemID(sys.size() - 1, system.generation);
}

void FBVClock::delete_clock_system(ClockSystemID cid) {
  ClockSystem &system = getSystem(cid);
  system.clock_indices.clear();
  system.dependencies.clear();
  system.idx_to_id.clear();
  system.allocated = false;
}

FBVClock::FBVClock(ClockSystemID system_id, int dimension) : cid(system_id) {
  if (dimension < 0)
    throw std::out_of_range("FBVClock dimension must be non-negative");

  ClockSystem &system = getSystem(cid);
  idx = static_cast<std::size_t>(dimension);
  if (idx >= system.idx_to_id.size())
    system.idx_to_id.resize(idx + 1, INVALID_ID);
  if (system.idx_to_id[idx] != INVALID_ID)
    throw std::invalid_argument("FBVClock dimension already has an owner");

  id = system.clock_indices.size();
  system.clock_indices.push_back(idx);
  system.dependencies.emplace_back();
  system.idx_to_id[idx] = id;
}

FBVClock::FBVClock(FBVClock &&other) noexcept
    : cid(other.cid), id(other.id), idx(other.idx) {
  other.invalidate();
}

FBVClock &FBVClock::operator=(FBVClock &&other) noexcept {
  if (this == &other)
    return *this;
  cid = other.cid;
  id = other.id;
  idx = other.idx;
  other.invalidate();
  return *this;
}

bool FBVClock::operator[](int dimension) const {
  if (dimension < 0)
    throw std::out_of_range("FBVClock dimension must be non-negative");
  return (*this)[static_cast<std::size_t>(dimension)];
}

bool FBVClock::operator[](std::size_t dimension) const {
  const std::vector<bool> clock = snapshot();
  return dimension < clock.size() && clock[dimension];
}

FBVClock &FBVClock::operator+=(const FBVClock &c) {
  ClockSystem &system = getSystem();
  c.getSystem();
  if (cid != c.cid)
    throw std::invalid_argument("cannot join FBVClocks from different systems");
  system.dependencies[id].insert(c.id);
  return *this;
}

std::size_t FBVClock::size() const { return getSystem().idx_to_id.size(); }

void FBVClock::invalidate() noexcept {
  cid = ClockSystemID();
  id = INVALID_ID;
  idx = INVALID_ID;
}

FBVClock::ClockSystem &FBVClock::getSystem(ClockSystemID system_id) {
  if (system_id.slot >= sys.size())
    throw std::logic_error("invalid FBVClock system handle");
  ClockSystem &system = sys[system_id.slot];
  if (!system.allocated || system.generation != system_id.generation)
    throw std::logic_error("stale FBVClock system handle");
  return system;
}

const FBVClock::ClockSystem &FBVClock::getSystem() const {
  const ClockSystem &system = getSystem(cid);
  if (id >= system.clock_indices.size() || id >= system.dependencies.size() ||
      system.clock_indices[id] != idx)
    throw std::logic_error("invalid FBVClock handle");
  return system;
}

FBVClock::ClockSystem &FBVClock::getSystem() {
  return const_cast<ClockSystem &>(
      static_cast<const FBVClock &>(*this).getSystem());
}

std::vector<bool> FBVClock::snapshot() const {
  const ClockSystem &system = getSystem();
  std::vector<bool> clock(system.idx_to_id.size(), false);
  std::vector<std::size_t> worklist{id};
  std::vector<bool> visited(system.clock_indices.size(), false);

  while (!worklist.empty()) {
    const std::size_t current = worklist.back();
    worklist.pop_back();
    if (current >= system.clock_indices.size())
      throw std::logic_error("corrupt FBVClock dependency");
    if (visited[current])
      continue;
    visited[current] = true;
    clock[system.clock_indices[current]] = true;
    for (std::size_t dependency : system.dependencies[current])
      worklist.push_back(dependency);
  }
  return clock;
}

std::string FBVClock::to_string() const {
  const std::vector<bool> clock = snapshot();
  std::string result(clock.size(), '0');
  for (std::size_t i = 0; i < clock.size(); ++i) {
    if (clock[i])
      result[i] = '1';
  }
  return result;
}
