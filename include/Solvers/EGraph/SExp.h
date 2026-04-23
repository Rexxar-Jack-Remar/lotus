#pragma once

#include "Solvers/EGraph/Util.h"

namespace lotus::egraph {

class SExp {
public:
  enum class Kind { Atom, List };

  SExp() : kind_(Kind::Atom) {}
  explicit SExp(std::string atom) : kind_(Kind::Atom), atom_(std::move(atom)) {}
  explicit SExp(std::vector<SExp> list)
      : kind_(Kind::List), list_(std::move(list)) {}

  Kind kind() const { return kind_; }
  bool isAtom() const { return kind_ == Kind::Atom; }
  bool isList() const { return kind_ == Kind::List; }

  const std::string &atom() const { return atom_; }
  const std::vector<SExp> &list() const { return list_; }

  std::string toString() const {
    if (isAtom()) {
      return atom_;
    }

    std::vector<std::string> parts;
    parts.reserve(list_.size());
    for (const auto &item : list_) {
      parts.push_back(item.toString());
    }
    return "(" + joinStrings(parts, " ") + ")";
  }

  static SExp parse(std::string_view input) {
    size_t offset = 0;
    auto expr = parseOne(input, offset);
    skipWhitespace(input, offset);
    if (offset != input.size()) {
      throw std::runtime_error("Trailing input after s-expression");
    }
    return expr;
  }

private:
  static void skipWhitespace(std::string_view input, size_t &offset) {
    while (offset < input.size() &&
           std::isspace(static_cast<unsigned char>(input[offset]))) {
      ++offset;
    }
  }

  static SExp parseOne(std::string_view input, size_t &offset) {
    skipWhitespace(input, offset);
    if (offset >= input.size()) {
      throw std::runtime_error("Unexpected end of input while parsing s-expression");
    }

    if (input[offset] == '(') {
      ++offset;
      std::vector<SExp> items;
      for (;;) {
        skipWhitespace(input, offset);
        if (offset >= input.size()) {
          throw std::runtime_error("Unclosed list in s-expression");
        }
        if (input[offset] == ')') {
          ++offset;
          return SExp(std::move(items));
        }
        items.push_back(parseOne(input, offset));
      }
    }

    if (input[offset] == ')') {
      throw std::runtime_error("Unexpected ')' in s-expression");
    }

    size_t start = offset;
    while (offset < input.size()) {
      char ch = input[offset];
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(' ||
          ch == ')') {
        break;
      }
      ++offset;
    }
    return SExp(std::string(input.substr(start, offset - start)));
  }

  Kind kind_;
  std::string atom_;
  std::vector<SExp> list_;
};

inline std::ostream &operator<<(std::ostream &os, const SExp &sexp) {
  os << sexp.toString();
  return os;
}

} // namespace lotus::egraph
