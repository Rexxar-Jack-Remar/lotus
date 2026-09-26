#pragma once

#include "Dataflow/NPA/Core/Domain.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

template <class Op, class OpLess = std::less<Op>> struct TransformerLess {
  bool operator()(const std::vector<Op> &lhs,
                  const std::vector<Op> &rhs) const {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end(), OpLess{});
  }
};

template <class Op, class OpLess = std::less<Op>>
class TransformerSummaryValue {
public:
  using transformer_type = std::vector<Op>;
  using transformer_set =
      std::set<transformer_type, TransformerLess<Op, OpLess>>;
  using write_set = std::unordered_set<const void *>;

  struct Storage {
    transformer_set transformers;
    bool overflow = false;
    write_set may_write;

    bool operator==(const Storage &other) const {
      return overflow == other.overflow &&
             transformers == other.transformers &&
             may_write == other.may_write;
    }
  };

  TransformerSummaryValue() = default;

  explicit TransformerSummaryValue(std::shared_ptr<const Storage> storage)
      : storage_(std::move(storage)) {}

  const transformer_set &transformers() const {
    static const transformer_set empty;
    return storage_ ? storage_->transformers : empty;
  }

  bool overflow() const { return storage_ && storage_->overflow; }

  const write_set &mayWrites() const {
    static const write_set empty;
    return storage_ ? storage_->may_write : empty;
  }

  bool mayWrite(const void *value) const {
    return storage_ && storage_->may_write.count(value) != 0;
  }

  bool operator==(const TransformerSummaryValue &other) const {
    if (storage_ == other.storage_)
      return true;
    if (!storage_ || !other.storage_)
      return false;
    return *storage_ == *other.storage_;
  }

  bool operator!=(const TransformerSummaryValue &other) const {
    return !(*this == other);
  }

private:
  template <class, class> friend class TransformerSummary;
  std::shared_ptr<const Storage> storage_;
};

/// Bounded, immutable abstract-summary domain for subdistributive analyses.
template <class Op, class OpLess = std::less<Op>> class TransformerSummary {
public:
  using value_type = TransformerSummaryValue<Op, OpLess>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool sparse_npa_zero_left_annihilator = true;
  static constexpr bool sparse_npa_zero_right_annihilator = true;
  static constexpr std::size_t max_transformers = 4096;
  static constexpr std::size_t max_transformer_length = 320;

  static value_type zero() { return {}; }

  static value_type one() {
    typename value_type::Storage storage;
    storage.transformers.insert(typename value_type::transformer_type{});
    return freeze(std::move(storage));
  }

  static value_type singleton(const Op &op) {
    typename value_type::Storage storage;
    insertTransformer(storage, typename value_type::transformer_type{op});
    return freeze(std::move(storage));
  }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }

  static value_type combine(const value_type &a, const value_type &b) {
    if (equal(a, b))
      return a;
    if (isZero(a))
      return b;
    if (isZero(b))
      return a;

    const value_type *base = &a;
    const value_type *added = &b;
    if (b.transformers().size() > a.transformers().size())
      std::swap(base, added);

    typename value_type::Storage storage = *base->storage_;
    storage.overflow = a.overflow() || b.overflow();
    storage.may_write.insert(added->mayWrites().begin(),
                             added->mayWrites().end());
    for (const auto &transformer : added->transformers())
      insertTransformer(storage, transformer);
    return freeze(std::move(storage));
  }

  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  static value_type extend(const value_type &a, const value_type &b) {
    if (isZero(a) || isZero(b))
      return zero();
    if (isOne(a))
      return b;
    if (isOne(b))
      return a;

    typename value_type::Storage storage;
    storage.overflow = a.overflow() || b.overflow();
    storage.may_write.insert(a.mayWrites().begin(), a.mayWrites().end());
    storage.may_write.insert(b.mayWrites().begin(), b.mayWrites().end());
    for (const auto &inner : b.transformers()) {
      for (const auto &outer : a.transformers()) {
        typename value_type::transformer_type composed;
        composed.reserve(inner.size() + outer.size());
        composed.insert(composed.end(), inner.begin(), inner.end());
        composed.insert(composed.end(), outer.begin(), outer.end());
        insertTransformer(storage, std::move(composed));
      }
    }
    return freeze(std::move(storage));
  }

  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }

  static value_type subtract(const value_type &a, const value_type &) {
    return a;
  }

private:
  template <typename... Ts> using void_t = void;

  template <typename T, typename = void>
  struct HasPointerDest : std::false_type {};

  template <typename T>
  struct HasPointerDest<T, void_t<decltype(std::declval<T>().dest)>>
      : std::integral_constant<
            bool, std::is_pointer<decltype(std::declval<T>().dest)>::value> {};

  template <typename T, typename = void>
  struct HasSummaryCanBeOverwritten : std::false_type {};

  template <typename T>
  struct HasSummaryCanBeOverwritten<
      T, void_t<decltype(std::declval<const T &>().summaryCanBeOverwritten())>>
      : std::true_type {};

  template <typename T, typename = void>
  struct HasSummaryCanOverwritePrevious : std::false_type {};

  template <typename T>
  struct HasSummaryCanOverwritePrevious<
      T,
      void_t<decltype(std::declval<const T &>().summaryCanOverwritePrevious())>>
      : std::true_type {};

  static bool isZero(const value_type &value) { return !value.storage_; }

  static bool isOne(const value_type &value) {
    return !value.overflow() && value.mayWrites().empty() &&
           value.transformers().size() == 1 &&
           value.transformers().begin()->empty();
  }

  static value_type freeze(typename value_type::Storage storage) {
    if (!storage.overflow && storage.transformers.empty() &&
        storage.may_write.empty()) {
      return zero();
    }
    return value_type(
        std::make_shared<const typename value_type::Storage>(
            std::move(storage)));
  }

  template <typename T = Op>
  static typename std::enable_if<HasPointerDest<T>::value, void>::type
  noteWrite(typename value_type::Storage &storage, const T &op) {
    if (op.dest)
      storage.may_write.insert(op.dest);
  }

  template <typename T = Op>
  static typename std::enable_if<!HasPointerDest<T>::value, void>::type
  noteWrite(typename value_type::Storage &, const T &) {}

  static typename value_type::transformer_type
  canonicalizeTransformer(typename value_type::transformer_type transformer) {
    typename value_type::transformer_type result;
    result.reserve(transformer.size());
    for (const auto &op : transformer) {
      if (!result.empty() && canFoldTrailingWrite(result.back(), op))
        result.back() = op;
      else
        result.push_back(op);
    }
    return result;
  }

  template <typename T = Op>
  static typename std::enable_if<HasPointerDest<T>::value, bool>::type
  canFoldTrailingWrite(const T &previous, const T &current) {
    return previous.dest && previous.dest == current.dest &&
           summaryCanBeOverwritten(previous) &&
           summaryCanOverwritePrevious(current);
  }

  template <typename T = Op>
  static typename std::enable_if<!HasPointerDest<T>::value, bool>::type
  canFoldTrailingWrite(const T &, const T &) {
    return false;
  }

  template <typename T = Op>
  static
      typename std::enable_if<HasSummaryCanBeOverwritten<T>::value, bool>::type
      summaryCanBeOverwritten(const T &op) {
    return op.summaryCanBeOverwritten();
  }

  template <typename T = Op>
  static typename std::enable_if<!HasSummaryCanBeOverwritten<T>::value,
                                 bool>::type
  summaryCanBeOverwritten(const T &) {
    return false;
  }

  template <typename T = Op>
  static typename std::enable_if<HasSummaryCanOverwritePrevious<T>::value,
                                 bool>::type
  summaryCanOverwritePrevious(const T &op) {
    return op.summaryCanOverwritePrevious();
  }

  template <typename T = Op>
  static typename std::enable_if<!HasSummaryCanOverwritePrevious<T>::value,
                                 bool>::type
  summaryCanOverwritePrevious(const T &) {
    return false;
  }

  static void insertTransformer(
      typename value_type::Storage &storage,
      typename value_type::transformer_type transformer) {
    transformer = canonicalizeTransformer(std::move(transformer));
    for (const auto &op : transformer)
      noteWrite(storage, op);
    if (transformer.size() > max_transformer_length) {
      storage.overflow = true;
      return;
    }
    if (storage.transformers.size() >= max_transformers &&
        !storage.transformers.count(transformer)) {
      storage.overflow = true;
      return;
    }
    storage.transformers.insert(std::move(transformer));
  }
};

} // namespace npa

