#ifndef __FBVCLOCK_H__
#define __FBVCLOCK_H__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/Support/raw_ostream.h>

/// A live dependency clock over one-shot boolean dimensions.
///
/// Unlike BVClock, joining an FBVClock records a dependency on the source.
/// Later growth of that source is therefore visible through the destination.
/// FBVClock is a move-only handle; use BVClock assignment for a value snapshot.
class FBVClock {
public:
  class ClockSystemID {
  public:
    ClockSystemID() = default;

    bool operator==(const ClockSystemID &other) const {
      return slot == other.slot && generation == other.generation;
    }
    bool operator!=(const ClockSystemID &other) const {
      return !(*this == other);
    }

  private:
    friend class FBVClock;
    ClockSystemID(std::size_t slot, std::uint64_t generation)
        : slot(slot), generation(generation) {}

    std::size_t slot = std::numeric_limits<std::size_t>::max();
    std::uint64_t generation = 0;
  };

  static ClockSystemID new_clock_system();
  static void delete_clock_system(ClockSystemID cid);

  FBVClock(ClockSystemID cid, int idx);
  FBVClock(const FBVClock &) = delete;
  FBVClock &operator=(const FBVClock &) = delete;
  FBVClock(FBVClock &&other) noexcept;
  FBVClock &operator=(FBVClock &&other) noexcept;

  bool operator[](int i) const;
  bool operator[](std::size_t i) const;
  FBVClock &operator+=(const FBVClock &c);
  std::string to_string() const;
  void invalidate() noexcept;

  /* Returns some natural number i such that for all j s.t. i<=j, it
   * holds that (*this)[j] == false.
   *
   * In particular i will be the number of clock elements currently
   * kept track of by this clock.
   */
  std::size_t size() const;

private:
  static constexpr std::size_t INVALID_ID =
      std::numeric_limits<std::size_t>::max();

  struct ClockSystem {
    bool allocated = false;
    std::uint64_t generation = 0;
    std::vector<std::size_t> clock_indices;
    std::vector<std::unordered_set<std::size_t>> dependencies;
    std::vector<std::size_t> idx_to_id;
  };

  ClockSystemID cid;
  std::size_t id = INVALID_ID;
  std::size_t idx = INVALID_ID;

  static std::vector<ClockSystem> sys;

  static ClockSystem &getSystem(ClockSystemID cid);
  const ClockSystem &getSystem() const;
  ClockSystem &getSystem();
  std::vector<bool> snapshot() const;
};

inline std::ostream &operator<<(std::ostream &os, const FBVClock &c) {
  return os << c.to_string();
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     const FBVClock &c) {
  return os << c.to_string();
}

#endif
