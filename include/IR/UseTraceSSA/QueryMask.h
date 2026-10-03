#pragma once

#include <algorithm>
#include <iterator>
#include <variant>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/SmallBitVector.h>
#include <llvm/ADT/SmallVector.h>

namespace lotus {
namespace usetracessa {
namespace detail {

/// Query-local finite mask. Small universes fit inline. Sparse subsets and
/// sparse exceptions to the universe avoid allocating a bitmap per delta or
/// event partition. These complements never change graph ObjectSet semantics.
class AdaptiveMask {
  using Dense = llvm::SmallBitVector;
  struct Sparse {
    std::vector<unsigned> values;
    bool inverted = false;
    bool operator==(const Sparse &other) const {
      return inverted == other.inverted && values == other.values;
    }
  };
  std::variant<Dense, Sparse> Bits;
  unsigned Size;
  bool sparse() const { return std::holds_alternative<Sparse>(Bits); }
  const Sparse &list() const { return std::get<Sparse>(Bits); }
  enum class Operation { Intersect, Unite, Subtract };
  static std::vector<unsigned> combine(const std::vector<unsigned> &a,
                                       const std::vector<unsigned> &b,
                                       Operation operation) {
    std::vector<unsigned> result;
    if (operation == Operation::Intersect)
      std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                            std::back_inserter(result));
    else if (operation == Operation::Unite)
      std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                     std::back_inserter(result));
    else
      std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(result));
    return result;
  }

public:
  explicit AdaptiveMask(unsigned size, bool all = false)
      : Bits(Dense(size, all)), Size(size) {}
  static AdaptiveMask sparse(unsigned size) {
    AdaptiveMask result(0);
    result.Size = size;
    result.Bits = Sparse{};
    return result;
  }
  unsigned size() const { return Size; }
  bool empty() const { return Size == 0; }
  unsigned count() const {
    return sparse() ? (list().inverted ? Size - list().values.size()
                                       : list().values.size())
                    : std::get<Dense>(Bits).count();
  }
  bool none() const {
    return sparse() ? count() == 0 : std::get<Dense>(Bits).none();
  }
  bool all() const { return count() == Size; }
  bool isSmall() const { return !sparse() && std::get<Dense>(Bits).isSmall(); }
  llvm::ArrayRef<uintptr_t> getData(uintptr_t &storage) const {
    return std::get<Dense>(Bits).getData(storage);
  }
  int find_first() const {
    if (!sparse())
      return std::get<Dense>(Bits).find_first();
    if (!list().inverted)
      return list().values.empty() ? -1 : int(list().values.front());
    unsigned bit = 0;
    for (auto excluded : list().values) {
      if (excluded != bit)
        break;
      ++bit;
    }
    return bit == Size ? -1 : int(bit);
  }
  bool test(unsigned bit) const {
    return sparse()
               ? list().inverted != std::binary_search(list().values.begin(),
                                                       list().values.end(), bit)
               : std::get<Dense>(Bits).test(bit);
  }
  void set(unsigned bit) {
    if (!sparse()) {
      std::get<Dense>(Bits).set(bit);
      return;
    }
    auto &data = std::get<Sparse>(Bits);
    auto found = std::lower_bound(data.values.begin(), data.values.end(), bit);
    if (data.inverted) {
      if (found != data.values.end() && *found == bit)
        data.values.erase(found);
    } else if (found == data.values.end() || *found != bit)
      data.values.insert(found, bit);
  }
  AdaptiveMask complemented() const {
    AdaptiveMask result = *this;
    if (result.sparse())
      std::get<Sparse>(result.Bits).inverted = !list().inverted;
    else
      std::get<Dense>(result.Bits).flip();
    return result;
  }
  // Whether this mask has bits outside other, without allocating a difference.
  bool test(const AdaptiveMask &other) const {
    if (!sparse() && !other.sparse())
      return std::get<Dense>(Bits).test(std::get<Dense>(other.Bits));
    if (count() > other.count())
      return true;
    if (sparse() && !list().inverted) {
      for (auto bit : list().values)
        if (!other.test(bit))
          return true;
      return false;
    }
    if (sparse() && other.sparse() && list().inverted && other.list().inverted)
      return !std::includes(list().values.begin(), list().values.end(),
                            other.list().values.begin(),
                            other.list().values.end());
    if (!sparse() && other.sparse() && other.list().inverted) {
      for (auto bit : other.list().values)
        if (test(bit))
          return true;
      return false;
    }
    if (sparse() && !other.sparse()) {
      Dense zeros = std::get<Dense>(other.Bits);
      zeros.flip();
      for (auto bit : zeros.set_bits())
        if (test(bit))
          return true;
      return false;
    }
    for (unsigned bit = 0; bit < Size; ++bit)
      if (test(bit) && !other.test(bit))
        return true;
    return false;
  }
  bool anyCommon(const AdaptiveMask &other) const {
    if (sparse() && !list().inverted) {
      for (auto bit : list().values)
        if (other.test(bit))
          return true;
      return false;
    }
    if (other.sparse() && !other.list().inverted)
      return other.anyCommon(*this);
    if (sparse() && other.sparse())
      return Size > list().values.size() + other.list().values.size() ||
             Size >
                 combine(list().values, other.list().values, Operation::Unite)
                     .size();
    if (sparse()) {
      if (other.count() > list().values.size())
        return true;
      for (auto bit : std::get<Dense>(other.Bits).set_bits())
        if (test(bit))
          return true;
      return false;
    }
    if (other.sparse())
      return other.anyCommon(*this);
    uintptr_t aStorage, bStorage;
    auto a = getData(aStorage), b = other.getData(bStorage);
    for (std::size_t i = 0; i < a.size(); ++i)
      if (a[i] & b[i])
        return true;
    return false;
  }
  void normalize() {
    const auto limit = Size > 256 ? Size / 128 : 0;
    if (sparse() && list().values.size() > limit) {
      Dense dense(Size, list().inverted);
      for (auto bit : list().values) {
        if (list().inverted)
          dense.reset(bit);
        else
          dense.set(bit);
      }
      Bits = std::move(dense);
    } else if (!sparse() && limit) {
      auto ones = count();
      if (std::min(ones, Size - ones) > limit)
        return;
      Dense selected = std::get<Dense>(Bits);
      bool inverted = ones > Size / 2;
      if (inverted)
        selected.flip();
      Sparse result;
      result.inverted = inverted;
      for (auto bit : selected.set_bits())
        result.values.push_back(bit);
      Bits = std::move(result);
    }
  }
  bool operator==(const AdaptiveMask &other) const {
    if (Size != other.Size)
      return false;
    if (sparse() == other.sparse())
      return Bits == other.Bits;
    return count() == other.count() && !test(other);
  }
  AdaptiveMask &operator&=(const AdaptiveMask &other) {
    if (sparse() && other.sparse()) {
      bool a = list().inverted, b = other.list().inverted;
      auto values =
          a && b ? combine(list().values, other.list().values, Operation::Unite)
          : a ? combine(other.list().values, list().values, Operation::Subtract)
          : b ? combine(list().values, other.list().values, Operation::Subtract)
              : combine(list().values, other.list().values,
                        Operation::Intersect);
      Bits = Sparse{std::move(values), a && b};
    } else if (!sparse() && !other.sparse()) {
      std::get<Dense>(Bits) &= std::get<Dense>(other.Bits);
    } else {
      const auto &data = sparse() ? list() : other.list();
      const auto &filter = sparse() ? other : *this;
      if (data.inverted) {
        Dense result = std::get<Dense>(filter.Bits);
        for (auto bit : data.values)
          result.reset(bit);
        Bits = std::move(result);
      } else {
        Sparse result;
        for (auto bit : data.values)
          if (filter.test(bit))
            result.values.push_back(bit);
        Bits = std::move(result);
      }
    }
    return *this;
  }
  AdaptiveMask &operator|=(const AdaptiveMask &other) {
    if (sparse() && other.sparse()) {
      bool a = list().inverted, b = other.list().inverted;
      auto values =
          a && b ? combine(list().values, other.list().values,
                           Operation::Intersect)
          : a ? combine(list().values, other.list().values, Operation::Subtract)
          : b ? combine(other.list().values, list().values, Operation::Subtract)
              : combine(list().values, other.list().values, Operation::Unite);
      Bits = Sparse{std::move(values), a || b};
    } else if (!sparse() && !other.sparse()) {
      std::get<Dense>(Bits) |= std::get<Dense>(other.Bits);
    } else {
      const auto &data = sparse() ? list() : other.list();
      const auto &dense = std::get<Dense>(sparse() ? other.Bits : Bits);
      if (data.inverted) {
        Sparse result;
        result.inverted = true;
        for (auto bit : data.values)
          if (!dense.test(bit))
            result.values.push_back(bit);
        Bits = std::move(result);
      } else {
        Dense result = dense;
        for (auto bit : data.values)
          result.set(bit);
        Bits = std::move(result);
      }
    }
    return *this;
  }
  AdaptiveMask &reset(const AdaptiveMask &other) {
    if (sparse() && other.sparse()) {
      bool a = list().inverted, b = other.list().inverted;
      auto values =
          a && b
              ? combine(other.list().values, list().values, Operation::Subtract)
          : a ? combine(list().values, other.list().values, Operation::Unite)
          : b ? combine(list().values, other.list().values,
                        Operation::Intersect)
              : combine(list().values, other.list().values,
                        Operation::Subtract);
      Bits = Sparse{std::move(values), a && !b};
    } else if (!sparse() && !other.sparse()) {
      std::get<Dense>(Bits).reset(std::get<Dense>(other.Bits));
    } else if (sparse()) {
      if (list().inverted) {
        Dense result = std::get<Dense>(other.Bits);
        result.flip();
        for (auto bit : list().values)
          result.reset(bit);
        Bits = std::move(result);
      } else {
        Sparse result;
        for (auto bit : list().values)
          if (!other.test(bit))
            result.values.push_back(bit);
        Bits = std::move(result);
      }
    } else if (other.list().inverted) {
      Sparse result;
      for (auto bit : other.list().values)
        if (test(bit))
          result.values.push_back(bit);
      Bits = std::move(result);
    } else {
      for (auto bit : other.list().values)
        std::get<Dense>(Bits).reset(bit);
    }
    return *this;
  }
  llvm::BitVector bitVector() const {
    llvm::BitVector result(Size, sparse() && list().inverted);
    if (sparse()) {
      for (auto bit : list().values) {
        if (list().inverted)
          result.reset(bit);
        else
          result.set(bit);
      }
    } else if (Size) {
      uintptr_t storage;
      llvm::SmallVector<uint32_t, 2> halves;
      for (auto word : getData(storage)) {
        halves.push_back(static_cast<uint32_t>(word));
        if (sizeof(uintptr_t) == 8)
          halves.push_back(static_cast<uint64_t>(word) >> 32);
      }
      result.setBitsInMask(halves.data(), (Size + 31) / 32);
    }
    return result;
  }
};

} // namespace detail
} // namespace usetracessa
} // namespace lotus
