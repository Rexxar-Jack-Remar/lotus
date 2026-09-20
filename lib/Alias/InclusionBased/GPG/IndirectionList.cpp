#include "Alias/InclusionBased/GPG/IndirectionList.h"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace lotus::gpg {

Indirection Indirection::dereference() {
  return {IndirectionKind::Dereference, 0};
}

Indirection Indirection::fieldAt(std::int64_t field) {
  return {IndirectionKind::Field, field};
}

Indirection Indirection::anyField() { return {IndirectionKind::AnyField, 0}; }

bool Indirection::matches(const Indirection &other) const {
  if (kind == IndirectionKind::AnyField ||
      other.kind == IndirectionKind::AnyField)
    return true;
  return *this == other;
}

bool Indirection::operator==(const Indirection &other) const {
  return kind == other.kind &&
         (kind != IndirectionKind::Field || field == other.field);
}

bool Indirection::operator<(const Indirection &other) const {
  return std::tie(kind, field) < std::tie(other.kind, other.field);
}

IndirectionList::IndirectionList(std::vector<Indirection> elements,
                                 bool summarized)
    : elements_(std::move(elements)), summarized_(summarized) {}

IndirectionList IndirectionList::dereferences(unsigned count) {
  return IndirectionList(
      std::vector<Indirection>(count, Indirection::dereference()));
}

IndirectionList IndirectionList::singletonField(std::int64_t field) {
  return IndirectionList({Indirection::fieldAt(field)});
}

bool IndirectionList::isPrefixOf(const IndirectionList &other) const {
  if (size() > other.size())
    return false;

  for (std::size_t i = 0; i < size(); ++i) {
    if (!elements_[i].matches(other.elements_[i]))
      return false;
  }

  // A summarized prefix already denotes an unbounded suffix. It can only be
  // balanced against another summarized path with the same stored prefix.
  if (summarized_)
    return other.summarized_ && size() == other.size();
  return true;
}

bool IndirectionList::isProperPrefixOf(const IndirectionList &other) const {
  if (!isPrefixOf(other))
    return false;
  return size() < other.size() || (!summarized_ && other.summarized_);
}

bool IndirectionList::equivalentTo(const IndirectionList &other) const {
  return isPrefixOf(other) && other.isPrefixOf(*this);
}

bool IndirectionList::doesNotExceed(const IndirectionList &other) const {
  if (size() != other.size())
    return size() < other.size();
  return !summarized_ || other.summarized_;
}

IndirectionList IndirectionList::limited(std::vector<Indirection> elements,
                                         bool summarized, unsigned k_limit) {
  if (k_limit != 0 && elements.size() >= k_limit) {
    elements.resize(k_limit);
    summarized = true;
  }
  return IndirectionList(std::move(elements), summarized);
}

IndirectionList IndirectionList::append(const IndirectionList &suffix,
                                        unsigned k_limit) const {
  if (summarized_)
    return limited(elements_, true, k_limit);

  std::vector<Indirection> result = elements_;
  result.insert(result.end(), suffix.elements_.begin(), suffix.elements_.end());
  return limited(std::move(result), suffix.summarized_, k_limit);
}

std::vector<IndirectionList>
IndirectionList::remaindersAfter(const IndirectionList &prefix,
                                 unsigned k_limit) const {
  if (!prefix.isPrefixOf(*this))
    return {};

  std::vector<Indirection> suffix(elements_.begin() + prefix.size(),
                                  elements_.end());
  if (!summarized_)
    return {IndirectionList(std::move(suffix))};

  std::vector<IndirectionList> result;
  const std::size_t expansion = std::max<std::size_t>(1, prefix.size());
  for (std::size_t count = 0; count <= expansion; ++count) {
    std::vector<Indirection> candidate = suffix;
    candidate.insert(candidate.end(), count, Indirection::anyField());
    const bool still_summarized = count == expansion;
    IndirectionList list =
        limited(std::move(candidate), still_summarized, k_limit);
    if (std::find(result.begin(), result.end(), list) == result.end())
      result.push_back(std::move(list));
  }
  return result;
}

std::string IndirectionList::str() const {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < elements_.size(); ++i) {
    if (i != 0)
      out << ',';
    switch (elements_[i].kind) {
    case IndirectionKind::Dereference:
      out << '*';
      break;
    case IndirectionKind::Field:
      out << elements_[i].field;
      break;
    case IndirectionKind::AnyField:
      out << "dagger";
      break;
    }
  }
  if (summarized_)
    out << ",...";
  out << ']';
  return out.str();
}

bool IndirectionList::operator==(const IndirectionList &other) const {
  return summarized_ == other.summarized_ && elements_ == other.elements_;
}

bool IndirectionList::operator<(const IndirectionList &other) const {
  return std::tie(elements_, summarized_) <
         std::tie(other.elements_, other.summarized_);
}

} // namespace lotus::gpg
