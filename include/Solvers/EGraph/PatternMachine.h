#pragma once

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

namespace detail {

template <typename L> struct Reg {
  uint32_t value = 0;

  friend bool operator==(Reg lhs, Reg rhs) { return lhs.value == rhs.value; }
  friend bool operator<(Reg lhs, Reg rhs) { return lhs.value < rhs.value; }
};

template <typename L> struct ENodeOrReg {
  std::variant<L, Reg<L>> value;

  explicit ENodeOrReg(const L &node) : value(node) {}
  explicit ENodeOrReg(Reg<L> reg) : value(reg) {}

  bool isNode() const { return std::holds_alternative<L>(value); }
  const L &node() const { return std::get<L>(value); }
  Reg<L> reg() const { return std::get<Reg<L>>(value); }
};

template <typename L> struct Instruction {
  enum class Kind { Bind, Compare, Lookup, Scan };

  Kind kind = Kind::Scan;
  std::optional<L> node;
  std::vector<ENodeOrReg<L>> term;
  Reg<L> i{};
  Reg<L> j{};
  Reg<L> out{};

  static Instruction bind(const L &node, Reg<L> i, Reg<L> out) {
    Instruction inst;
    inst.kind = Kind::Bind;
    inst.node = node;
    inst.i = i;
    inst.out = out;
    return inst;
  }

  static Instruction compare(Reg<L> i, Reg<L> j) {
    Instruction inst;
    inst.kind = Kind::Compare;
    inst.i = i;
    inst.j = j;
    return inst;
  }

  static Instruction lookup(std::vector<ENodeOrReg<L>> term, Reg<L> i) {
    Instruction inst;
    inst.kind = Kind::Lookup;
    inst.term = std::move(term);
    inst.i = i;
    return inst;
  }

  static Instruction scan(Reg<L> out) {
    Instruction inst;
    inst.kind = Kind::Scan;
    inst.out = out;
    return inst;
  }
};

template <typename L> class Machine {
public:
  void reserve(size_t registers) {
    regs_.reserve(registers);
    ancestry_heads_.reserve(registers);
    lookup_.reserve(registers);
  }

  template <typename A, typename Yield>
  void run(const EGraph<L, A> &egraph,
           const std::vector<Instruction<L>> &instructions,
           const Subst &subst_template, const WorkControl *control,
           Yield &&yield_fn) {
    reject_cycles_ = !egraph.analysis().allowEMatchingCycles();
    control_ = control;
    runFrom(egraph, instructions, 0, subst_template,
            std::forward<Yield>(yield_fn));
  }

  void seed(Id id) {
    regs_.clear();
    ancestry_heads_.clear();
    ancestry_nodes_.clear();
    regs_.push_back(id);
    ancestry_heads_.push_back(NO_ANCESTOR);
  }

private:
  template <typename A, typename Yield>
  bool runFrom(const EGraph<L, A> &egraph,
               const std::vector<Instruction<L>> &instructions, size_t pc,
               const Subst &subst_template, Yield &&yield_fn) {
    if (pc >= instructions.size()) {
      return yield_fn(*this, subst_template);
    }
    if (control_ && control_->poll()) {
      return false;
    }

    const auto &inst = instructions[pc];
    switch (inst.kind) {
    case Instruction<L>::Kind::Bind: {
      if (!inst.node) {
        throw std::logic_error("Bind instruction is missing its language node");
      }
      const auto &klass = egraph[reg(inst.i)];
      return forEachMatchingNode(klass, *inst.node, [&](const L &matched) {
        const Id parent = egraph.find(reg(inst.i));
        const size_t parent_head = ancestry_heads_.at(inst.i.value);
        if (regs_.size() > inst.out.value) {
          regs_.resize(inst.out.value);
          ancestry_heads_.resize(inst.out.value);
        }
        const size_t old_ancestry_size = ancestry_nodes_.size();
        size_t child_head = NO_ANCESTOR;
        if (reject_cycles_ && !matched.children().empty()) {
          child_head = ancestry_nodes_.size();
          ancestry_nodes_.push_back({parent, parent_head});
        }
        bool cyclic = false;
        for (Id child : matched.children()) {
          regs_.push_back(child);
          Id canonical_child = egraph.find(child);
          if (reject_cycles_ &&
              (canonical_child == parent ||
               ancestryContains(parent_head, canonical_child))) {
            cyclic = true;
          }
          ancestry_heads_.push_back(child_head);
        }
        if (cyclic) {
          ancestry_nodes_.resize(old_ancestry_size);
          return true;
        }
        bool keep_going =
            runFrom(egraph, instructions, pc + 1, subst_template, yield_fn);
        ancestry_nodes_.resize(old_ancestry_size);
        return keep_going;
      });
    }
    case Instruction<L>::Kind::Compare:
      if (egraph.find(reg(inst.i)) != egraph.find(reg(inst.j))) {
        return true;
      }
      return runFrom(egraph, instructions, pc + 1, subst_template, yield_fn);
    case Instruction<L>::Kind::Lookup: {
      lookup_.clear();
      for (const auto &entry : inst.term) {
        if (entry.isNode()) {
          auto rebuilt = entry.node().mapChildren(
              [&](Id child) { return lookup_.at(child.index()); });
          auto found = egraph.lookup(rebuilt);
          if (!found) {
            return true;
          }
          lookup_.push_back(*found);
        } else {
          lookup_.push_back(egraph.find(reg(entry.reg())));
        }
      }
      if (lookup_.empty() || lookup_.back() != egraph.find(reg(inst.i))) {
        return true;
      }
      return runFrom(egraph, instructions, pc + 1, subst_template, yield_fn);
    }
    case Instruction<L>::Kind::Scan:
      for (Id id : egraph.classIds()) {
        if (regs_.size() > inst.out.value) {
          regs_.resize(inst.out.value);
          ancestry_heads_.resize(inst.out.value);
        }
        regs_.push_back(id);
        ancestry_heads_.push_back(NO_ANCESTOR);
        if (!runFrom(egraph, instructions, pc + 1, subst_template, yield_fn)) {
          return false;
        }
      }
      return true;
    }
    return true;
  }

  Id reg(Reg<L> reg) const { return regs_.at(reg.value); }

public:
  const std::vector<Id> &regs() const { return regs_; }

private:
  struct AncestryNode {
    Id id;
    size_t parent = NO_ANCESTOR;
  };

  static constexpr size_t NO_ANCESTOR = std::numeric_limits<size_t>::max();

  bool ancestryContains(size_t head, Id id) const {
    while (head != NO_ANCESTOR) {
      const auto &node = ancestry_nodes_[head];
      if (node.id == id) {
        return true;
      }
      head = node.parent;
    }
    return false;
  }

  std::vector<Id> regs_;
  std::vector<size_t> ancestry_heads_;
  std::vector<AncestryNode> ancestry_nodes_;
  std::vector<Id> lookup_;
  bool reject_cycles_ = false;
  const WorkControl *control_ = nullptr;
};

template <typename L> class Compiler {
public:
  void compile(std::optional<Var> binder, const PatternAst<L> &pattern) {
    loadPattern(pattern);
    Id root = pattern.root();
    Reg<L> next_out = next_reg_;

    auto add_new_pattern = [&](Compiler &self) {
      if (!self.instructions_.empty()) {
        self.instructions_.push_back(Instruction<L>::scan(self.next_reg_));
      }
      self.addTodo(pattern, root, self.next_reg_);
    };

    if (binder) {
      auto it = v2r_.find(*binder);
      if (it != v2r_.end()) {
        addTodo(pattern, root, it->second);
      } else {
        ++next_out.value;
        add_new_pattern(*this);
        v2r_[*binder] = next_reg_;
      }
    } else {
      ++next_out.value;
      add_new_pattern(*this);
    }

    while (!todo_nodes_.empty()) {
      auto [key, node] = nextTodo();
      Id id = key.first;
      Reg<L> reg = key.second;
      if (isGroundNow(id) && !node.children().empty()) {
        auto extracted = pattern.extract(id);
        std::vector<ENodeOrReg<L>> term;
        term.reserve(extracted.size());
        for (const auto &entry : extracted.items()) {
          if (entry.isNode()) {
            term.emplace_back(entry.node());
          } else {
            term.emplace_back(v2r_.at(entry.var()));
          }
        }
        instructions_.push_back(Instruction<L>::lookup(std::move(term), reg));
      } else {
        Reg<L> out = next_out;
        next_out.value += static_cast<uint32_t>(node.children().size());
        auto op = node.mapChildren([](Id) { return Id::fromIndex(0); });
        instructions_.push_back(Instruction<L>::bind(op, reg, out));
        for (size_t i = 0; i < node.children().size(); ++i) {
          addTodo(pattern, node.children()[i],
                  Reg<L>{out.value + static_cast<uint32_t>(i)});
        }
      }
    }

    next_reg_ = next_out;
  }

  std::pair<std::vector<Instruction<L>>, Subst> extract() && {
    Subst subst = Subst::withCapacity(v2r_.size());
    for (const auto &[var, reg] : v2r_) {
      subst.insert(var, Id::fromIndex(reg.value));
    }
    return {std::move(instructions_), std::move(subst)};
  }

private:
  void addTodo(const PatternAst<L> &pattern, Id id, Reg<L> reg) {
    const auto &entry = pattern[id];
    if (entry.isVar()) {
      auto it = v2r_.find(entry.var());
      if (it != v2r_.end()) {
        instructions_.push_back(Instruction<L>::compare(reg, it->second));
      } else {
        v2r_[entry.var()] = reg;
      }
      return;
    }
    auto key = std::make_pair(id, reg);
    for (auto &todo : todo_nodes_) {
      if (todo.first == key) {
        todo.second = entry.node();
        return;
      }
    }
    todo_nodes_.push_back({key, entry.node()});
  }

  void loadPattern(const PatternAst<L> &pattern) {
    free_vars_.clear();
    subtree_size_.clear();
    free_vars_.reserve(pattern.size());
    subtree_size_.reserve(pattern.size());

    for (const auto &entry : pattern.items()) {
      std::vector<Var> free;
      size_t size = 0;
      if (entry.isVar()) {
        free.push_back(entry.var());
      } else {
        size = 1;
        for (Id child : entry.node().children()) {
          const auto &child_vars = free_vars_.at(child.index());
          free.insert(free.end(), child_vars.begin(), child_vars.end());
          size += subtree_size_.at(child.index());
        }
        std::sort(free.begin(), free.end());
        free.erase(std::unique(free.begin(), free.end()), free.end());
      }
      free_vars_.push_back(std::move(free));
      subtree_size_.push_back(size);
    }
  }

  bool isGroundNow(Id id) const {
    for (const auto &var : free_vars_.at(id.index())) {
      if (!v2r_.count(var)) {
        return false;
      }
    }
    return true;
  }

  std::pair<std::pair<Id, Reg<L>>, L> nextTodo() {
    size_t best = 0;
    auto score = [&](const auto &entry) {
      Id id = entry.first.first;
      size_t index = id.index();
      size_t n_bound = 0;
      for (const auto &var : free_vars_.at(index)) {
        if (v2r_.count(var)) {
          ++n_bound;
        }
      }
      size_t n_free = free_vars_.at(index).size() - n_bound;
      return std::tuple<bool, size_t, long long>(
          n_free == 0, n_free,
          -static_cast<long long>(subtree_size_.at(index)));
    };
    for (size_t i = 1; i < todo_nodes_.size(); ++i) {
      if (score(todo_nodes_[i]) > score(todo_nodes_[best])) {
        best = i;
      }
    }

    auto value = std::move(todo_nodes_[best]);
    todo_nodes_.erase(todo_nodes_.begin() + best);
    return value;
  }

  std::unordered_map<Var, Reg<L>> v2r_;
  std::vector<std::vector<Var>> free_vars_;
  std::vector<size_t> subtree_size_;
  std::vector<std::pair<std::pair<Id, Reg<L>>, L>> todo_nodes_;
  std::vector<Instruction<L>> instructions_;
  Reg<L> next_reg_{};
};

} // namespace detail

template <typename L> class PatternProgram {
public:
  PatternProgram() = default;

  static PatternProgram compileFromPattern(const Pattern<L> &pattern) {
    detail::Compiler<L> compiler;
    compiler.compile(std::nullopt, pattern.ast());
    auto [instructions, subst] = std::move(compiler).extract();
    return PatternProgram(std::move(instructions), std::move(subst));
  }

  static PatternProgram compileFromMultiPattern(
      const std::vector<std::pair<Var, PatternAst<L>>> &patterns) {
    detail::Compiler<L> compiler;
    for (const auto &[var, pattern] : patterns) {
      compiler.compile(var, pattern);
    }
    auto [instructions, subst] = std::move(compiler).extract();
    return PatternProgram(std::move(instructions), std::move(subst));
  }

  template <typename A>
  std::vector<Subst> runWithLimit(const EGraph<L, A> &egraph, Id eclass,
                                  size_t limit,
                                  const WorkControl *control = nullptr) const {
    if (!egraph.clean()) {
      throw std::runtime_error("Tried to search a dirty e-graph");
    }
    if (limit == 0) {
      return {};
    }

    detail::Machine<L> machine;
    machine.reserve(register_count_);
    return runWithMachine(egraph, eclass, limit, control, machine);
  }

  template <typename A>
  std::vector<SearchMatches<L>>
  runEClassesWithLimit(const EGraph<L, A> &egraph,
                       const std::vector<Id> &eclasses, size_t limit,
                       const WorkControl *control,
                       std::shared_ptr<const PatternAst<L>> ast) const {
    std::vector<SearchMatches<L>> results;
    detail::Machine<L> machine;
    machine.reserve(register_count_);
    for (Id eclass : eclasses) {
      if (limit == 0 || (control && control->poll())) {
        break;
      }
      auto substs = runWithMachine(egraph, eclass, limit, control, machine);
      if (substs.empty()) {
        continue;
      }
      limit -= std::min(limit, substs.size());
      results.push_back(SearchMatches<L>{eclass, std::move(substs), ast});
    }
    return results;
  }

private:
  template <typename A>
  std::vector<Subst> runWithMachine(const EGraph<L, A> &egraph, Id eclass,
                                    size_t limit, const WorkControl *control,
                                    detail::Machine<L> &machine) const {
    machine.seed(eclass);
    std::vector<Subst> matches;
    machine.run(egraph, instructions_, subst_template_, control,
                [&](const auto &machine_state, const Subst &template_subst) {
                  Subst subst =
                      Subst::withCapacity(template_subst.bindings().size());
                  for (const auto &[var, reg_id] : template_subst.bindings()) {
                    subst.insert(var, machine_state.regs().at(reg_id.index()));
                  }
                  matches.push_back(std::move(subst));
                  return matches.size() < limit;
                });
    return matches;
  }

  PatternProgram(std::vector<detail::Instruction<L>> instructions, Subst subst)
      : instructions_(std::move(instructions)),
        subst_template_(std::move(subst)) {
    register_count_ = 1;
    for (const auto &instruction : instructions_) {
      register_count_ =
          std::max(register_count_, static_cast<size_t>(instruction.out.value) +
                                        (instruction.node
                                             ? instruction.node->children().size()
                                             : 0));
    }
  }

  std::vector<detail::Instruction<L>> instructions_;
  Subst subst_template_;
  size_t register_count_ = 1;
};

} // namespace lotus::egraph
