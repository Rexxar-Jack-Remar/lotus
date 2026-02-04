// Chunked sparse bitset points-to set.
//
// Representation:
// - A sorted vector of fixed-size chunks.
// - Each chunk stores kChunkBits bits in kWords 64-bit words.
// - Operations are linear in the number of non-empty chunks.
//
// This file is standalone and not wired into any analysis yet.
#ifndef ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H
#define ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

class ChunkedSparseBitsetPtsSet {
public:
  using Index = std::uint64_t;

  static constexpr std::size_t kChunkBits = 1024;
  static constexpr std::size_t kWordBits = 64;
  static constexpr std::size_t kWords = kChunkBits / kWordBits;

  static_assert(kChunkBits % kWordBits == 0,
                "Chunk size must be a multiple of word size.");

  struct Chunk {
    std::uint64_t id = 0;
    Index base = 0;
    std::array<std::uint64_t, kWords> words{};

    bool operator==(const Chunk &other) const {
      return id == other.id && words == other.words;
    }
  };

  class iterator {
  public:
    using value_type = Index;
    using difference_type = std::ptrdiff_t;
    using pointer = const Index *;
    using reference = const Index &;
    using iterator_category = std::forward_iterator_tag;

    iterator() = default;

    Index operator*() { return current_; }

    iterator &operator++() {
      advance();
      return *this;
    }

    bool operator==(const iterator &other) const {
      return chunks_ == other.chunks_ && chunk_pos_ == other.chunk_pos_ &&
             word_pos_ == other.word_pos_ && bit_pos_ == other.bit_pos_ &&
             end_ == other.end_;
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    friend class ChunkedSparseBitsetPtsSet;

    explicit iterator(const std::vector<Chunk> *chunks, bool end)
        : chunks_(chunks), end_(end) {
      if (chunks_ && !end_) {
        chunk_pos_ = 0;
        word_pos_ = 0;
        bit_pos_ = 0;
        findNext();
      }
    }

    void advance() {
      if (end_)
        return;
      ++bit_pos_;
      findNext();
    }

    void findNext() {
      if (!chunks_ || chunks_->empty()) {
        end_ = true;
        return;
      }

      while (chunk_pos_ < chunks_->size()) {
        const auto &chunk = (*chunks_)[chunk_pos_];
        while (word_pos_ < kWords) {
          std::uint64_t word = chunk.words[word_pos_];
          if (word == 0) {
            ++word_pos_;
            bit_pos_ = 0;
            continue;
          }

          for (; bit_pos_ < kWordBits; ++bit_pos_) {
            const std::uint64_t mask = std::uint64_t{1} << bit_pos_;
            if (word & mask) {
              current_ = chunk.base +
                         static_cast<Index>(word_pos_ * kWordBits + bit_pos_);
              return;
            }
          }

          ++word_pos_;
          bit_pos_ = 0;
        }

        ++chunk_pos_;
        word_pos_ = 0;
        bit_pos_ = 0;
      }

      end_ = true;
    }

    const std::vector<Chunk> *chunks_ = nullptr;
    std::size_t chunk_pos_ = 0;
    std::size_t word_pos_ = 0;
    std::size_t bit_pos_ = 0;
    Index current_ = 0;
    bool end_ = true;
  };

  // Return true if *this has idx as an element.
  bool has(Index idx) const {
    const std::uint64_t chunk_id = chunkId(idx);
    const std::uint64_t offset = chunkOffset(idx);
    const std::size_t word = static_cast<std::size_t>(offset / kWordBits);
    const std::size_t bit = static_cast<std::size_t>(offset % kWordBits);

    const std::size_t pos = findChunk(chunk_id);
    if (pos == chunks_.size() || chunks_[pos].id != chunk_id)
      return false;
    return (chunks_[pos].words[word] >> bit) & 1U;
  }

  // Return true if the set changes.
  bool insert(Index idx) {
    const std::uint64_t chunk_id = chunkId(idx);
    const std::uint64_t offset = chunkOffset(idx);
    const std::size_t word = static_cast<std::size_t>(offset / kWordBits);
    const std::size_t bit = static_cast<std::size_t>(offset % kWordBits);

    const std::size_t pos = findChunk(chunk_id);
    if (pos == chunks_.size() || chunks_[pos].id != chunk_id) {
      Chunk chunk;
      chunk.id = chunk_id;
      chunk.base = chunkBase(chunk_id);
      chunk.words.fill(0);
      chunk.words[word] |= (std::uint64_t{1} << bit);
      chunks_.insert(chunks_.begin() + static_cast<std::ptrdiff_t>(pos), chunk);
      return true;
    }

    std::uint64_t &word_ref = chunks_[pos].words[word];
    const std::uint64_t mask = std::uint64_t{1} << bit;
    if (word_ref & mask)
      return false;
    word_ref |= mask;
    return true;
  }

  bool contains(const ChunkedSparseBitsetPtsSet &other) const {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < chunks_.size() && j < other.chunks_.size()) {
      const auto &lhs = chunks_[i];
      const auto &rhs = other.chunks_[j];
      if (lhs.id < rhs.id) {
        ++i;
        continue;
      }
      if (lhs.id > rhs.id)
        return false;
      for (std::size_t w = 0; w < kWords; ++w) {
        if ((lhs.words[w] & rhs.words[w]) != rhs.words[w])
          return false;
      }
      ++i;
      ++j;
    }
    return j == other.chunks_.size();
  }

  bool intersectWith(const ChunkedSparseBitsetPtsSet &other) const {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < chunks_.size() && j < other.chunks_.size()) {
      const auto &lhs = chunks_[i];
      const auto &rhs = other.chunks_[j];
      if (lhs.id < rhs.id) {
        ++i;
        continue;
      }
      if (lhs.id > rhs.id) {
        ++j;
        continue;
      }
      for (std::size_t w = 0; w < kWords; ++w) {
        if ((lhs.words[w] & rhs.words[w]) != 0)
          return true;
      }
      ++i;
      ++j;
    }
    return false;
  }

  // Return true if the set changes.
  bool unionWith(const ChunkedSparseBitsetPtsSet &other) {
    if (other.isEmpty())
      return false;

    bool changed = false;
    std::vector<Chunk> merged;
    merged.reserve(chunks_.size() + other.chunks_.size());

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < chunks_.size() || j < other.chunks_.size()) {
      if (j == other.chunks_.size() ||
          (i < chunks_.size() && chunks_[i].id < other.chunks_[j].id)) {
        merged.push_back(chunks_[i++]);
        continue;
      }
      if (i == chunks_.size() || other.chunks_[j].id < chunks_[i].id) {
        merged.push_back(other.chunks_[j++]);
        changed = true;
        continue;
      }
      Chunk out = chunks_[i];
      for (std::size_t w = 0; w < kWords; ++w) {
        const std::uint64_t combined = out.words[w] | other.chunks_[j].words[w];
        changed |= (combined != out.words[w]);
        out.words[w] = combined;
      }
      merged.push_back(out);
      ++i;
      ++j;
    }

    if (changed)
      chunks_.swap(merged);
    return changed;
  }

  void clear() { chunks_.clear(); }

  unsigned getSize() const {
    std::uint64_t total = 0;
    for (const auto &chunk : chunks_) {
      for (std::uint64_t word : chunk.words)
        total += popcount(word);
    }
    return static_cast<unsigned>(total);
  }

  bool isEmpty() const { return chunks_.empty(); }

  bool operator==(const ChunkedSparseBitsetPtsSet &other) const {
    return chunks_ == other.chunks_;
  }

  iterator begin() const { return iterator(&chunks_, false); }
  iterator end() const { return iterator(&chunks_, true); }

private:
  static std::uint64_t chunkId(Index idx) {
    return static_cast<std::uint64_t>(idx / kChunkBits);
  }

  static std::uint64_t chunkOffset(Index idx) {
    return static_cast<std::uint64_t>(idx % kChunkBits);
  }

  static Index chunkBase(std::uint64_t chunk_id) {
    return static_cast<Index>(chunk_id * kChunkBits);
  }

  std::size_t findChunk(std::uint64_t chunk_id) const {
    std::size_t lo = 0;
    std::size_t hi = chunks_.size();
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (chunks_[mid].id < chunk_id)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  static unsigned popcount(std::uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<unsigned>(__builtin_popcountll(value));
#else
    unsigned count = 0;
    while (value) {
      value &= (value - 1);
      ++count;
    }
    return count;
#endif
  }

  std::vector<Chunk> chunks_;
};

#endif // ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H
