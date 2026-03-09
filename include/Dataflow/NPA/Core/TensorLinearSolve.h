#ifndef NPA_TENSOR_LINEAR_SOLVE_H
#define NPA_TENSOR_LINEAR_SOLVE_H

/**
 * \file
 * \brief Tensor-product linear solver (Reps et al. TOPLAS 2016, Alg. 3.4).
 *
 * Converts the LCFL linear system into a \e left-linear system over the
 * paired semiring (TensorProductDomain): pair (a,b) represents left/right
 * context so that a·Y·b becomes Y ⊗_p (a,b). The left-linear system is
 * solved by worklist (or could use Tarjan path expressions); then we
 * \e project back to the base domain (readout R((w1,w2)) = w1⊗w2).
 * Regularization requires Concat coefficients to be constant; otherwise we
 * fall back to worklist.
 *
 * The implementation uses an exact correlated representation in tensor space
 * for idempotent domains. This avoids correlation loss at projection time, at
 * the cost of potentially larger intermediate values.
 */

#include "Dataflow/NPA/Core/LinearSolvers.h"
#include "Dataflow/NPA/Core/TensorDiff.h"
#include "Dataflow/NPA/Core/TensorSemiring.h"
#include "Utils/Algorithms/PathExpressions/PathExpressions.h"

#include <mutex>
#include <sstream>

namespace npa {

/// Constant evaluator for Exp1: succeeds only if expression is variable-free.
template <class D> struct Exp1ConstEval {
  using V = DomVal<D>;
  using E = E1<D>;
  static Optional<V> eval(const E &e) {
    std::unordered_map<Symbol, V> env;
    return eval_with_env(e, env);
  }

private:
  static Optional<V> eval_with_env(const E &e,
                                   const std::unordered_map<Symbol, V> &env) {
    if (!e)
      return {};
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term: {
      Optional<V> out;
      out = e->c;
      return out;
    }
    case K::Seq: {
      auto t = eval_with_env(e->t, env);
      if (!t.has_value())
        return {};
      Optional<V> out;
      out = D::extend_lin(e->c, *t);
      return out;
    }
    case K::SeqR: {
      auto t = eval_with_env(e->t, env);
      if (!t.has_value())
        return {};
      Optional<V> out;
      out = D::extend_lin(*t, e->c);
      return out;
    }
    case K::Add: {
      auto a = eval_with_env(e->t1, env), b = eval_with_env(e->t2, env);
      if (!a.has_value() || !b.has_value())
        return {};
      Optional<V> out;
      out = D::combine(*a, *b);
      return out;
    }
    case K::Sub: {
      if (!DomainHasSubtract<D>::value)
        return {};
      auto a = eval_with_env(e->t1, env), b = eval_with_env(e->t2, env);
      if (!a.has_value() || !b.has_value())
        return {};
      Optional<V> out;
      out = D::subtract(*a, *b);
      return out;
    }
    case K::Ndet: {
      auto a = eval_with_env(e->t1, env), b = eval_with_env(e->t2, env);
      if (!a.has_value() || !b.has_value())
        return {};
      Optional<V> out;
      out = D::ndetCombine(*a, *b);
      return out;
    }
    case K::Project: {
      auto t = eval_with_env(e->t, env);
      if (!t.has_value())
        return {};
      Optional<V> out;
      out = domain_project<D>(*t);
      return out;
    }
    case K::Cond: {
      auto t = eval_with_env(e->t1, env), f = eval_with_env(e->t2, env);
      if (!t.has_value() || !f.has_value())
        return {};
      Optional<V> out;
      out = D::condCombine(e->phi, *t, *f);
      return out;
    }
    case K::Bound: {
      auto it = env.find(e->sym);
      if (it == env.end())
        return {};
      Optional<V> out;
      out = it->second;
      return out;
    }
    case K::InfClos: {
      V fixed = fix<D>(false, D::zero(), [&](const V &cur) {
        auto env2 = env;
        env2[e->sym] = cur;
        auto body = eval_with_env(e->t, env2);
        return body.has_value() ? *body : D::zero();
      });
      auto env2 = env;
      env2[e->sym] = fixed;
      auto body = eval_with_env(e->t, env2);
      if (!body.has_value())
        return {};
      Optional<V> out;
      out = fixed;
      return out;
    }
    default:
      return {};
    }
  }
};

/// Converts linearized expression over D to a left-linear expression over
/// TensorProductDomain<D> by rewriting Concat (a·X·b) into X ⊗_p (a,b).
template <class D> struct Exp1ToTensor {
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  using E1D = E1<D>;
  using E1T = E1<TD>;
  static bool is_tensor_convertible(const E1D &e) {
    if (!e)
      return true;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Sub:
      return false;
    case K::Concat: {
      auto a = Exp1ConstEval<D>::eval(e->t1);
      auto b = Exp1ConstEval<D>::eval(e->t2);
      return a.has_value() && b.has_value();
    }
    default:
      break;
    }
    if (e->t && !is_tensor_convertible(e->t))
      return false;
    if (e->t1 && !is_tensor_convertible(e->t1))
      return false;
    if (e->t2 && !is_tensor_convertible(e->t2))
      return false;
    return true;
  }
  static bool is_regularizable(const E1D &e) {
    if (!e)
      return true;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Sub:
      // Tensor regularization is defined for semiring expressions; explicit
      // subtraction is outside that fragment and must stay on the base solver.
      return false;
    case K::InfClos:
      // The Tarjan extractor can only consume InfClos when it constant-folds
      // to a tensor-side coefficient. Non-constant starred fragments still
      // require the tensor worklist solver.
      return Exp1ConstEval<D>::eval(e).has_value();
    case K::Concat: {
      auto a = Exp1ConstEval<D>::eval(e->t1);
      auto b = Exp1ConstEval<D>::eval(e->t2);
      return a.has_value() && b.has_value();
    }
    case K::Project:
      return is_regularizable(e->t);
    default:
      break;
    }
    if (e->t && !is_regularizable(e->t))
      return false;
    if (e->t1 && !is_regularizable(e->t1))
      return false;
    if (e->t2 && !is_regularizable(e->t2))
      return false;
    return true;
  }
  static E1T convert(const E1D &e) {
    if (!e)
      return nullptr;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term:
      return Exp1<TD>::term(Traits::right_constant(e->c));
    case K::Seq:
      // Base: c ⊗ t. Right-multiply by c^t ⊙ 1 so readout yields c ⊗ R(t).
      return Exp1<TD>::seqR(convert(e->t), Traits::left_constant(e->c));
    case K::SeqR:
      // Base: t ⊗ c. Right-multiply by 1^t ⊙ c so readout yields R(t) ⊗ c.
      return Exp1<TD>::seqR(convert(e->t), Traits::right_constant(e->c));
    case K::Call:
      // Base: f ⊗ c.
      return Exp1<TD>::seqR(Exp1<TD>::hole(e->sym),
                            Traits::right_constant(e->c));
    case K::Cond:
      return Exp1<TD>::cond(e->phi, convert(e->t1), convert(e->t2));
    case K::Add:
      return Exp1<TD>::add(convert(e->t1), convert(e->t2));
    case K::Sub:
      return Exp1<TD>::sub(convert(e->t1), convert(e->t2));
    case K::Ndet:
      return Exp1<TD>::ndet(convert(e->t1), convert(e->t2));
    case K::Project:
      return Exp1<TD>::project(convert(e->t));
    case K::Hole:
      return Exp1<TD>::hole(e->sym);
    case K::Bound:
      return Exp1<TD>::bound(e->sym);
    case K::Concat:
      // Regularization: a·X·b -> X ⊗_p (a,b) (TOPLAS 2016, Alg. 3.4).
      {
        auto a = Exp1ConstEval<D>::eval(e->t1);
        auto b = Exp1ConstEval<D>::eval(e->t2);
        return Exp1<TD>::seqR(Exp1<TD>::hole(e->sym), Traits::couple(*a, *b));
      }
    case K::InfClos:
      return Exp1<TD>::inf(convert(e->t), e->sym);
    default:
      return nullptr;
    }
  }
};

template <class TD>
class TensorRegexEvaluator final
    : public lotus::pathexpressions::IRegexVisitor<int, typename TD::value_type,
                                                   std::nullptr_t> {
public:
  using V = typename TD::value_type;

  explicit TensorRegexEvaluator(const std::vector<V> &labels) : labels_(labels) {}

  V visit(const lotus::pathexpressions::Union<int> &re, std::nullptr_t) override {
    return TD::combine(re.getFirst()->accept(*this),
                       re.getSecond()->accept(*this));
  }

  V visit(const lotus::pathexpressions::Concatenation<int> &re,
          std::nullptr_t) override {
    return TD::extend(re.getFirst()->accept(*this), re.getSecond()->accept(*this));
  }

  V visit(const lotus::pathexpressions::Star<int> &re, std::nullptr_t) override {
    const V inner = re.getInner()->accept(*this);
    return fix<TD>(false, TD::one(), [&](V cur) {
      return TD::combine(TD::one(), TD::extend(inner, cur));
    });
  }

  V visit(const lotus::pathexpressions::Literal<int> &re,
          std::nullptr_t) override {
    return labels_.at(static_cast<std::size_t>(re.getLetter()));
  }

  V visit(const lotus::pathexpressions::Epsilon<int> &, std::nullptr_t) override {
    return TD::one();
  }

  V visit(const lotus::pathexpressions::EmptySet<int> &,
          std::nullptr_t) override {
    return TD::zero();
  }

private:
  const std::vector<V> &labels_;
};

template <class TD> struct TensorLeftLinearFragment {
  using V = typename TD::value_type;
  V constant = TD::zero();
  std::vector<std::pair<Symbol, V>> terms;
};

template <class TD> struct TensorProjectionSystemDetector {
  using E = E1<TD>;

  static bool
  is_projection_equation_system(const std::vector<std::pair<Symbol, E>> &rhs) {
    bool saw_project = false;
    for (const auto &eqn : rhs) {
      if (has_nonconstant_nested_project(eqn.second, true))
        return false;
      if (eqn.second && eqn.second->k == Exp1<TD>::Project &&
          !Exp1ConstEval<TD>::eval(eqn.second).has_value()) {
        saw_project = true;
      }
    }
    return saw_project;
  }

private:
  static bool has_nonconstant_nested_project(const E &e, bool at_root) {
    if (!e)
      return false;
    if (Exp1ConstEval<TD>::eval(e).has_value())
      return false;
    using K = typename Exp1<TD>::K;
    if (e->k == K::Project) {
      if (!at_root)
        return true;
      return has_nonconstant_nested_project(e->t, false);
    }
    if (e->t && has_nonconstant_nested_project(e->t, false))
      return true;
    if (e->t1 && has_nonconstant_nested_project(e->t1, false))
      return true;
    if (e->t2 && has_nonconstant_nested_project(e->t2, false))
      return true;
    return false;
  }
};

template <class TD> struct TensorLeftLinearExtractor {
  using V = typename TD::value_type;
  using Frag = TensorLeftLinearFragment<TD>;

  static Optional<Frag> extract(const E1<TD> &e,
                                bool allow_project_pushdown = false,
                                bool at_root = true) {
    auto c = Exp1ConstEval<TD>::eval(e);
    if (c.has_value()) {
      Optional<Frag> out;
      Frag frag;
      frag.constant = *c;
      out = frag;
      return out;
    }

    using K = typename Exp1<TD>::K;
    switch (e->k) {
    case K::Hole: {
      Optional<Frag> out;
      Frag frag;
      frag.terms.emplace_back(e->sym, TD::one());
      out = frag;
      return out;
    }
    case K::SeqR: {
      auto inner = extract(e->t, allow_project_pushdown, false);
      if (!inner.has_value())
        return {};
      Frag frag = *inner;
      frag.constant = TD::extend(frag.constant, e->c);
      for (auto &term : frag.terms)
        term.second = TD::extend(term.second, e->c);
      Optional<Frag> out;
      out = frag;
      return out;
    }
    case K::Project: {
      if (!allow_project_pushdown || !at_root)
        return {};
      auto inner = extract(e->t, false, false);
      if (!inner.has_value())
        return {};
      return project_fragment(*inner);
    }
    case K::Add:
    case K::Ndet: {
      auto lhs = extract(e->t1, allow_project_pushdown, false);
      auto rhs = extract(e->t2, allow_project_pushdown, false);
      if (!lhs.has_value() || !rhs.has_value())
        return {};
      Frag frag;
      frag.constant = TD::combine((*lhs).constant, (*rhs).constant);
      frag.terms = (*lhs).terms;
      frag.terms.insert(frag.terms.end(), (*rhs).terms.begin(), (*rhs).terms.end());
      Optional<Frag> out;
      out = frag;
      return out;
    }
    default:
      return {};
    }
  }

private:
  template <class T = TD>
  static typename std::enable_if<DomainHasProjectT<T>::value, Optional<Frag>>::type
  project_fragment(const Frag &frag_in) {
    Frag frag = frag_in;
    frag.constant = T::projectT(frag.constant);
    for (auto &term : frag.terms)
      term.second = T::projectT(term.second);
    Optional<Frag> out;
    out = frag;
    return out;
  }

  template <class T = TD>
  static typename std::enable_if<!DomainHasProjectT<T>::value, Optional<Frag>>::type
  project_fragment(const Frag &) {
    return {};
  }
};

template <class TD> struct TensorTarjanPlan {
  using RegexRef = lotus::pathexpressions::RegexRef<int>;
  std::vector<RegexRef> regexes;
};

/// Build and cache the dependency-graph regex topology used by the Tarjan fast
/// path. This is the implementation's equivalent of Alg. 7.1's parameterized
/// regular-expression step: the cache stores the topology and symbol slots of
/// the left-linear dependency graph, while the current-round tensor labels are
/// supplied separately on each Newton round.
template <class TD>
Optional<TensorTarjanPlan<TD>>
get_tensor_tarjan_plan(bool verbose,
                       const std::vector<std::pair<Symbol, E1<TD>>> &rhs,
                       std::vector<typename TD::value_type> &labels_out) {
  using V = typename TD::value_type;
  using Frag = typename TensorLeftLinearExtractor<TD>::Frag;
  using Graph = lotus::pathexpressions::GenericLabeledGraph<int, int>;

  std::unordered_map<Symbol, int> sym_to_node;
  for (int i = 0; i < static_cast<int>(rhs.size()); ++i)
    sym_to_node[rhs[i].first] = i + 1;

  std::ostringstream signature;
  signature << rhs.size() << ';';
  std::vector<Frag> fragments;
  fragments.reserve(rhs.size());
  labels_out.clear();
  const bool projection_system =
      TensorProjectionSystemDetector<TD>::is_projection_equation_system(rhs);

  for (int i = 0; i < static_cast<int>(rhs.size()); ++i) {
    auto extracted =
        TensorLeftLinearExtractor<TD>::extract(rhs[i].second, projection_system);
    if (!extracted.has_value()) {
      if (verbose)
        std::cerr << "[tensor] expression not extractable to left-linear graph; "
                     "falling back to tensor worklist\n";
      return {};
    }
    fragments.push_back(*extracted);

    signature << "C:";
    for (const auto &term : (*extracted).terms) {
      auto it = sym_to_node.find(term.first);
      if (it == sym_to_node.end()) {
        if (verbose)
          std::cerr << "[tensor] unknown symbol in left-linear extraction; "
                       "falling back to tensor worklist\n";
        return {};
      }
      signature << "T:" << term.first << ';';
    }
    signature << '|';
  }

  for (const auto &frag : fragments) {
    labels_out.push_back(frag.constant);
    for (const auto &term : frag.terms)
      labels_out.push_back(term.second);
  }

  using Plan = TensorTarjanPlan<TD>;
  static std::mutex cache_mu;
  static std::unordered_map<std::string, Plan> cache;

  const std::string key = signature.str();
  {
    std::lock_guard<std::mutex> lock(cache_mu);
    auto it = cache.find(key);
    if (it != cache.end()) {
      Optional<Plan> out;
      out = it->second;
      return out;
    }
  }

  Graph graph;
  graph.addNode(0);
  for (int i = 0; i < static_cast<int>(rhs.size()); ++i)
    graph.addNode(i + 1);

  std::size_t next_label = 0;
  auto add_label = [&](const V &) {
    return static_cast<int>(next_label++);
  };

  for (int i = 0; i < static_cast<int>(rhs.size()); ++i) {
    const Frag &frag = fragments[static_cast<std::size_t>(i)];
    graph.addEdge(0, add_label(frag.constant), i + 1);
    for (const auto &term : frag.terms)
      graph.addEdge(sym_to_node.at(term.first), add_label(term.second), i + 1);
  }

  lotus::pathexpressions::PathExpressionComputer<int, int> computer(graph);
  Plan plan;
  plan.regexes.reserve(rhs.size());
  for (int i = 0; i < static_cast<int>(rhs.size()); ++i)
    plan.regexes.push_back(computer.exprBetween(0, i + 1));

  {
    std::lock_guard<std::mutex> lock(cache_mu);
    auto inserted = cache.emplace(key, plan);
    Optional<Plan> out;
    out = inserted.first->second;
    return out;
  }
}

template <class TD>
Optional<std::vector<typename TD::value_type>> solve_linear_tensor_tarjan_impl(
    bool verbose, const std::vector<std::pair<Symbol, E1<TD>>> &rhs,
    const std::vector<typename TD::value_type> &init) {
  using V = typename TD::value_type;
  for (const auto &seed : init) {
    if (!domain_equal<TD>(seed, TD::zero())) {
      if (verbose)
        std::cerr << "[tensor] non-zero initial seeds require iterative solving; "
                     "falling back to tensor worklist\n";
      return {};
    }
  }

  std::vector<V> labels;
  auto plan = get_tensor_tarjan_plan<TD>(verbose, rhs, labels);
  if (!plan.has_value())
    return {};

  TensorRegexEvaluator<TD> evaluator(labels);

  std::vector<V> out;
  out.reserve(rhs.size());
  for (int i = 0; i < static_cast<int>(rhs.size()); ++i) {
    auto regex = (*plan).regexes[static_cast<std::size_t>(i)];
    out.push_back(regex->accept(evaluator, nullptr));
  }

  Optional<std::vector<V>> solved;
  solved = out;
  return solved;
}

template <class D>
std::vector<DomVal<D>> solve_linear_tensorized_impl(
    bool verbose,
    const std::vector<std::pair<
        Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>> &rhs_tensor,
    std::vector<DomVal<D>> init) {
  validate_tensor_trait_api<D>();
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  using VT = typename TD::value_type;

  std::vector<VT> init_tensor;
  init_tensor.reserve(init.size());
  for (const auto &v : init)
    init_tensor.emplace_back(lift_base_value_to_tensor<D>(v));
  auto delta_tensor =
      solve_linear_tensor_tarjan_impl<TD>(verbose, rhs_tensor, init_tensor);
  if (!delta_tensor.has_value())
    delta_tensor = solve_linear_worklist_impl<TD>(verbose, rhs_tensor, init_tensor);
  std::vector<DomVal<D>> delta;
  delta.reserve((*delta_tensor).size());
  for (const auto &p : *delta_tensor)
    delta.push_back(Traits::readout(p));
  return delta;
}

/// Solve LCFL linear system by lifting to tensor space: convert RHS to
/// TensorProductDomain, solve (left-linear over pairs), project back via R.
template <class D>
std::vector<DomVal<D>>
solve_linear_tensor_impl(bool verbose,
                         const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                         std::vector<DomVal<D>> init) {
  validate_tensor_trait_api<D>();
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  if (!Traits::available()) {
    if (verbose)
      std::cerr << "[tensor] tensor traits unavailable for domain; "
                   "falling back to worklist\n";
    return solve_linear_worklist_impl<D>(verbose, rhs, init);
  }
  if (!Traits::paper_admissible()) {
    if (verbose)
      std::cerr << "[tensor] tensor traits are not paper-admissible; "
                   "falling back to worklist\n";
    return solve_linear_worklist_impl<D>(verbose, rhs, init);
  }
  for (const auto &p : rhs) {
    if (!Exp1ToTensor<D>::is_tensor_convertible(p.second)) {
      if (verbose)
        std::cerr << "[tensor] not tensor-convertible; falling back to worklist\n";
      return solve_linear_worklist_impl<D>(verbose, rhs, init);
    }
  }
  using VT = typename TD::value_type;
  std::vector<std::pair<Symbol, E1<TD>>> rhs_tensor;
  rhs_tensor.reserve(rhs.size());
  for (const auto &p : rhs)
    rhs_tensor.emplace_back(p.first, Exp1ToTensor<D>::convert(p.second));
  return solve_linear_tensorized_impl<D>(verbose, rhs_tensor, init);
}

} // namespace npa

#endif // NPA_TENSOR_LINEAR_SOLVE_H
