#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lotus::gpg {

enum class IndirectionKind : std::uint8_t {
  Dereference,
  Field,
  AnyField,
};

struct Indirection {
  IndirectionKind kind = IndirectionKind::Dereference;
  std::int64_t field = 0;

  static Indirection dereference();
  static Indirection fieldAt(std::int64_t field);
  static Indirection anyField();

  bool matches(const Indirection &other) const;
  bool operator==(const Indirection &other) const;
  bool operator<(const Indirection &other) const;
};

// An indirection list is the paper's field-sensitive replacement for an
// indirection level. A summarized list denotes its stored prefix followed by
// zero or more field-insensitive dereferences (Appendix B.3).
class IndirectionList {
public:
  IndirectionList() = default;
  explicit IndirectionList(std::vector<Indirection> elements,
                           bool summarized = false);

  static IndirectionList dereferences(unsigned count);
  static IndirectionList singletonField(std::int64_t field);

  const std::vector<Indirection> &elements() const { return elements_; }
  std::size_t size() const { return elements_.size(); }
  bool empty() const { return elements_.empty(); }
  bool isSummarized() const { return summarized_; }

  bool isPrefixOf(const IndirectionList &other) const;
  bool isProperPrefixOf(const IndirectionList &other) const;
  bool equivalentTo(const IndirectionList &other) const;
  bool doesNotExceed(const IndirectionList &other) const;

  IndirectionList append(const IndirectionList &suffix,
                         unsigned k_limit = 0) const;

  // Computes sRemainder(prefix, *this). An empty result means prefix is not
  // compatible with this list. A summarized receiver can produce several
  // remainders, exactly as in Appendix B.3.
  std::vector<IndirectionList> remaindersAfter(const IndirectionList &prefix,
                                               unsigned k_limit = 0) const;

  std::string str() const;

  bool operator==(const IndirectionList &other) const;
  bool operator<(const IndirectionList &other) const;

private:
  std::vector<Indirection> elements_;
  bool summarized_ = false;

  static IndirectionList limited(std::vector<Indirection> elements,
                                 bool summarized, unsigned k_limit);
};

} // namespace lotus::gpg
