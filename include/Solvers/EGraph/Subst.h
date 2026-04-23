#pragma once

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Util.h"

namespace lotus::egraph {

class Var {
public:
  Var() = default;
  explicit Var(Symbol name) : name_(std::move(name)) {}

  static Var parse(std::string_view text) {
    if (text.size() < 2 || text[0] != '?') {
      throw std::runtime_error("Pattern variable must start with '?'");
    }
    return Var(std::string(text));
  }

  const Symbol &name() const { return name_; }

  friend bool operator==(const Var &lhs, const Var &rhs) {
    return lhs.name_ == rhs.name_;
  }

  friend bool operator!=(const Var &lhs, const Var &rhs) { return !(lhs == rhs); }
  friend bool operator<(const Var &lhs, const Var &rhs) { return lhs.name_ < rhs.name_; }

private:
  Symbol name_;
};

inline std::ostream &operator<<(std::ostream &os, const Var &var) {
  os << var.name();
  return os;
}

class Subst {
public:
  void insert(const Var &var, Id id) {
    for (auto &[bound_var, bound_id] : bindings_) {
      if (bound_var == var) {
        bound_id = id;
        return;
      }
    }
    bindings_.push_back({var, id});
  }

  const Id *get(const Var &var) const {
    for (const auto &[bound_var, id] : bindings_) {
      if (bound_var == var) {
        return &id;
      }
    }
    return nullptr;
  }

  Id at(const Var &var) const {
    if (const Id *id = get(var)) {
      return *id;
    }
    throw std::runtime_error("Substitution variable not found");
  }

  const std::vector<std::pair<Var, Id>> &bindings() const { return bindings_; }

  friend bool operator==(const Subst &lhs, const Subst &rhs) {
    return lhs.bindings_ == rhs.bindings_;
  }

  friend bool operator!=(const Subst &lhs, const Subst &rhs) {
    return !(lhs == rhs);
  }

private:
  std::vector<std::pair<Var, Id>> bindings_;
};

} // namespace lotus::egraph

template <> struct std::hash<lotus::egraph::Var> {
  size_t operator()(const lotus::egraph::Var &var) const noexcept {
    return std::hash<lotus::egraph::Symbol>{}(var.name());
  }
};
