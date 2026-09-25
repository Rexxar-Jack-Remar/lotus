#pragma once

/**
 * \file
 * \brief Lazy DAG analysis and filtered differentiation for sparse Newton.
 *
 * The sparse plan stores only the source-to-target dependency skeleton up
 * front. When demand first reaches a target in a round, one memoized traversal
 * of its Exp0 DAG computes its value, source influence, and derivative DAG.
 * The same traversal lazily records unique DAG leaves. A cheap structural
 * slice of that derivative retains demanded sources without reevaluating
 * domain operations. The result also carries the exact reduced dependency
 * graph; an unchanged graph reuses its SCC partition in the next round.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"
#include "Dataflow/NPA/Solver/Newton/Errors.h"
#include "Dataflow/NPA/Solver/Options.h"
#include "Dataflow/NPA/Solver/Statistics.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

namespace detail {

template <class D> struct DomainHasSparseLeftZeroAnnihilator {
  template <class T>
  static auto test(int)
      -> decltype(T::sparse_npa_zero_left_annihilator, std::true_type{});
  template <class> static std::false_type test(...);

  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasSparseRightZeroAnnihilator {
  template <class T>
  static auto test(int)
      -> decltype(T::sparse_npa_zero_right_annihilator, std::true_type{});
  template <class> static std::false_type test(...);

  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> inline bool sparse_left_zero_annihilator(std::true_type) {
  return D::sparse_npa_zero_left_annihilator;
}

template <class D> inline bool sparse_left_zero_annihilator(std::false_type) {
  return false;
}

template <class D> inline bool sparse_right_zero_annihilator(std::true_type) {
  return D::sparse_npa_zero_right_annihilator;
}

template <class D> inline bool sparse_right_zero_annihilator(std::false_type) {
  return false;
}

} // namespace detail

/// Domain customization point used by sparse zero-context analysis.
template <class D> struct SparseNewtonZeroOracle {
  using V = DomVal<D>;

  static bool isZero(const V &value) {
    return domain_exact_equal<D>(value, D::zero());
  }

  static bool leftZeroAnnihilates() {
    return detail::sparse_left_zero_annihilator<D>(
        std::integral_constant<
            bool, detail::DomainHasSparseLeftZeroAnnihilator<D>::value>{});
  }

  static bool rightZeroAnnihilates() {
    return detail::sparse_right_zero_annihilator<D>(
        std::integral_constant<
            bool, detail::DomainHasSparseRightZeroAnnihilator<D>::value>{});
  }

  static bool leftMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && leftZeroAnnihilates();
  }

  static bool rightMultiplyIsZeroMap(const V &coefficient) {
    return isZero(coefficient) && rightZeroAnnihilates();
  }
};

namespace detail {

template <class D> struct SparseRoundMaterialization {
  std::vector<std::pair<Symbol, E1<D>>> rhs;
  std::vector<unsigned> dense_indices;
  std::vector<std::vector<unsigned>> dependencies;
  std::vector<std::vector<unsigned>> scc_partition;
  NewtonRoundStat stats;
};

template <class D> class SparseNewtonSystem {
public:
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  using Env = typename I0<D>::Environment;
  using SourceSet = std::unordered_set<unsigned>;

  SparseNewtonSystem(const std::vector<Eqn> &equations,
                     const std::vector<std::pair<Symbol, V>> &fixed_seed,
                     const ValidatedEquationSystem &validated)
      : symbol_to_index_(validated.symbol_to_index),
        targets_by_source_(equations.size()),
        target_indexes_(equations.size()) {
    symbols_.reserve(equations.size());
    equations_.reserve(equations.size());
    seed_.reserve(equations.size());
    seed_support_.assign(equations.size(), false);
    for (std::size_t target = 0; target < equations.size(); ++target) {
      symbols_.push_back(equations[target].first);
      equations_.push_back(equations[target].second);
      seed_.push_back(fixed_seed[target].second);
      seed_support_[target] =
          !SparseNewtonZeroOracle<D>::isZero(fixed_seed[target].second);
      for (unsigned source : validated.dependencies[target])
        targets_by_source_[source].push_back(static_cast<unsigned>(target));
    }
    static_active_ = discoverStaticReachability();
  }

  std::size_t occurrenceCount() const { return indexed_leaf_count_; }
  double occurrenceIndexTime() const { return lazy_index_time_; }

  SparseRoundMaterialization<D>
  buildRound(const std::vector<std::pair<Symbol, V>> &binds,
             NewtonRoundStrategy strategy) const {
    std::unordered_map<Symbol, V> nu;
    nu.reserve(binds.size());
    for (const auto &binding : binds)
      nu.insert_or_assign(binding.first, binding.second);

    const auto discovery_start = std::chrono::steady_clock::now();
    std::vector<bool> active(symbols_.size(), false);
    std::vector<SharedDerivative> derivatives(symbols_.size());
    long queries = 0;
    long retained = 0;

    auto ensureDerivative = [&](unsigned target) -> const DerivativeResult & {
      if (!derivatives[target]) {
        DerivativeBuildContext context;
        context.prune_zero = strategy == NewtonRoundStrategy::Sparse;

        std::optional<TargetIndex> new_index;
        if (!target_indexes_[target]) {
          new_index.emplace(symbols_.size());
          context.index = &*new_index;
        }

        derivatives[target] =
            differentiateExpression(equations_[target], nu, {}, {}, context, 0);
        if (new_index) {
          indexed_leaf_count_ += new_index->leaf_count;
          target_indexes_[target].emplace(std::move(*new_index));
        }
      }
      return *derivatives[target];
    };

    if (strategy == NewtonRoundStrategy::Static) {
      active = static_active_;
    } else {
      std::deque<unsigned> worklist;
      for (std::size_t source = 0; source < seed_support_.size(); ++source) {
        if (!seed_support_[source])
          continue;
        active[source] = true;
        worklist.push_back(static_cast<unsigned>(source));
      }

      while (!worklist.empty()) {
        const unsigned source = worklist.front();
        worklist.pop_front();
        for (unsigned target : targets_by_source_[source]) {
          const DerivativeResult &derivative = ensureDerivative(target);
          const TargetIndex &index = *target_indexes_[target];
          const long source_occurrences = index.source_counts[source];
          queries += source_occurrences;

          const bool keep = strategy == NewtonRoundStrategy::AlwaysMaybe ||
                            derivative.sources.count(source) != 0;
          if (!keep)
            continue;
          if (!active[target]) {
            active[target] = true;
            worklist.push_back(target);
          }
        }
      }
    }

    SparseRoundMaterialization<D> result;
    result.stats.discovery_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      discovery_start)
            .count();
    result.stats.queried_occurrences = queries;
    result.stats.active_coordinates =
        static_cast<int>(std::count(active.begin(), active.end(), true));

    const auto materialization_start = std::chrono::steady_clock::now();
    result.rhs.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    result.dense_indices.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    result.dependencies.reserve(
        static_cast<std::size_t>(result.stats.active_coordinates));
    long materialized_terms = 0;

    std::vector<int> reduced_index(symbols_.size(), -1);
    for (std::size_t target = 0; target < symbols_.size(); ++target) {
      if (!active[target])
        continue;
      reduced_index[target] = static_cast<int>(result.dense_indices.size());
      result.dense_indices.push_back(static_cast<unsigned>(target));
    }

    for (unsigned target : result.dense_indices) {
      const DerivativeResult &derivative = ensureDerivative(target);
      const TargetIndex &index = *target_indexes_[target];
      std::vector<unsigned> dependencies;
      std::vector<bool> allowed_sources(symbols_.size(), false);
      for (std::size_t source = 0; source < symbols_.size(); ++source) {
        if (!active[source])
          continue;
        if (!derivative.sources.count(static_cast<unsigned>(source))) {
          continue;
        }
        allowed_sources[source] = true;
        retained += index.source_counts[source];
        materialized_terms += index.source_counts[source];
        dependencies.push_back(static_cast<unsigned>(reduced_index[source]));
      }

      LinearSliceContext slice_context;
      E1<D> linear_terms = sliceDerivative(derivative.derivative,
                                           allowed_sources, slice_context);
      E1<D> rhs = Exp1<D>::term(seed_[target]);
      if (linear_terms)
        rhs = Exp1<D>::add(std::move(rhs), std::move(linear_terms));
      result.rhs.emplace_back(symbols_[target], std::move(rhs));
      result.dependencies.push_back(std::move(dependencies));
    }

    result.scc_partition =
        getSccPartition(result.dense_indices, result.dependencies);

    result.stats.materialized_derivative_terms = materialized_terms;
    result.stats.retained_occurrences = retained;
    result.stats.materialization_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      materialization_start)
            .count();
    return result;
  }

private:
  struct NodeScopeKey {
    const Exp0<D> *expression = nullptr;
    std::size_t scope = 0;

    bool operator==(const NodeScopeKey &other) const {
      return expression == other.expression && scope == other.scope;
    }
  };

  struct NodeScopeKeyHash {
    std::size_t operator()(const NodeScopeKey &key) const {
      const std::size_t pointer_hash =
          std::hash<const Exp0<D> *>{}(key.expression);
      return pointer_hash ^ (key.scope + static_cast<std::size_t>(0x9e3779b9) +
                             (pointer_hash << 6) + (pointer_hash >> 2));
    }
  };

  struct TargetIndex {
    explicit TargetIndex(std::size_t source_count)
        : source_counts(source_count, 0) {}

    std::vector<long> source_counts;
    std::size_t leaf_count = 0;
  };

  struct DerivativeResult {
    V value;
    SourceSet sources;
    E1<D> derivative;
  };

  using SharedDerivative = std::shared_ptr<const DerivativeResult>;

  struct DerivativeBuildContext {
    std::unordered_map<NodeScopeKey, SharedDerivative, NodeScopeKeyHash> memo;
    std::size_t next_scope = 1;
    bool prune_zero = false;
    TargetIndex *index = nullptr;
  };

  struct LinearSliceContext {
    std::unordered_map<const Exp1<D> *, E1<D>> memo;
  };

  enum class ZeroOperationKind { LeftMultiply, RightMultiply };

  struct ZeroOperation {
    ZeroOperationKind kind;
    const V *coefficient = nullptr;
  };

  std::unordered_map<Symbol, unsigned> symbol_to_index_;
  std::vector<Symbol> symbols_;
  std::vector<E0<D>> equations_;
  std::vector<V> seed_;
  std::vector<bool> seed_support_;
  std::vector<bool> static_active_;
  std::vector<std::vector<unsigned>> targets_by_source_;
  mutable std::vector<std::optional<TargetIndex>> target_indexes_;
  mutable std::size_t indexed_leaf_count_ = 0;
  mutable std::vector<unsigned> cached_scc_dense_indices_;
  mutable std::vector<std::vector<unsigned>> cached_scc_dependencies_;
  mutable std::vector<std::vector<unsigned>> cached_scc_partition_;
  double lazy_index_time_ = 0.0;

  unsigned sourceIndex(const Symbol &symbol) const {
    return symbol_to_index_.at(symbol);
  }

  std::vector<bool> discoverStaticReachability() const {
    std::vector<bool> active = seed_support_;
    std::deque<unsigned> worklist;
    for (std::size_t source = 0; source < active.size(); ++source) {
      if (active[source])
        worklist.push_back(static_cast<unsigned>(source));
    }
    while (!worklist.empty()) {
      const unsigned source = worklist.front();
      worklist.pop_front();
      for (unsigned target : targets_by_source_[source]) {
        if (!active[target]) {
          active[target] = true;
          worklist.push_back(target);
        }
      }
    }
    return active;
  }

  static std::vector<std::vector<unsigned>>
  computeSccPartition(const std::vector<std::vector<unsigned>> &dependencies) {
    const int count = static_cast<int>(dependencies.size());
    std::vector<int> index(static_cast<std::size_t>(count), -1);
    std::vector<int> low(static_cast<std::size_t>(count), -1);
    std::vector<int> scc_id(static_cast<std::size_t>(count), -1);
    std::vector<unsigned> stack;
    stack.reserve(static_cast<std::size_t>(count));
    int next_index = 0;
    int scc_count = 0;

    std::function<void(unsigned)> tarjan = [&](unsigned node) {
      index[node] = low[node] = next_index++;
      stack.push_back(node);
      for (unsigned dependency : dependencies[node]) {
        if (index[dependency] == -1) {
          tarjan(dependency);
          low[node] = std::min(low[node], low[dependency]);
        } else if (scc_id[dependency] == -1) {
          low[node] = std::min(low[node], index[dependency]);
        }
      }
      if (low[node] != index[node])
        return;
      for (;;) {
        const unsigned member = stack.back();
        stack.pop_back();
        scc_id[member] = scc_count;
        if (member == node)
          break;
      }
      ++scc_count;
    };

    for (unsigned node = 0; node < dependencies.size(); ++node) {
      if (index[node] == -1)
        tarjan(node);
    }

    std::vector<std::vector<unsigned>> partition(
        static_cast<std::size_t>(scc_count));
    for (unsigned node = 0; node < dependencies.size(); ++node) {
      partition[static_cast<std::size_t>(scc_id[node])].push_back(node);
    }
    return partition;
  }

  const std::vector<std::vector<unsigned>> &getSccPartition(
      const std::vector<unsigned> &dense_indices,
      const std::vector<std::vector<unsigned>> &dependencies) const {
    if (dense_indices != cached_scc_dense_indices_ ||
        dependencies != cached_scc_dependencies_) {
      cached_scc_dense_indices_ = dense_indices;
      cached_scc_dependencies_ = dependencies;
      cached_scc_partition_ = computeSccPartition(dependencies);
    }
    return cached_scc_partition_;
  }

  void noteLeaf(const Symbol &symbol, DerivativeBuildContext &context) const {
    if (!context.index)
      return;
    ++context.index->source_counts[sourceIndex(symbol)];
    ++context.index->leaf_count;
  }

  static void applyZeroOperation(std::optional<V> &constant,
                                 const ZeroOperation &operation) {
    if (operation.kind == ZeroOperationKind::LeftMultiply) {
      if (constant) {
        constant = D::extend(*operation.coefficient, *constant);
      } else if (SparseNewtonZeroOracle<D>::leftMultiplyIsZeroMap(
                     *operation.coefficient)) {
        constant = D::zero();
      }
      return;
    }

    if (constant) {
      constant = D::extend(*constant, *operation.coefficient);
    } else if (SparseNewtonZeroOracle<D>::rightMultiplyIsZeroMap(
                   *operation.coefficient)) {
      constant = D::zero();
    }
  }

  static bool contextIsZero(std::initializer_list<ZeroOperation> operations) {
    std::optional<V> constant;
    for (const ZeroOperation &operation : operations)
      applyZeroOperation(constant, operation);
    return constant && SparseNewtonZeroOracle<D>::isZero(*constant);
  }

  static ZeroOperation left(const V &coefficient) {
    return {ZeroOperationKind::LeftMultiply, &coefficient};
  }

  static ZeroOperation right(const V &coefficient) {
    return {ZeroOperationKind::RightMultiply, &coefficient};
  }

  static E1<D> combineLinear(E1<D> lhs, E1<D> rhs) {
    if (!lhs)
      return rhs;
    if (!rhs)
      return lhs;
    return Exp1<D>::add(std::move(lhs), std::move(rhs));
  }

  static void mergeSources(SourceSet &destination, const SourceSet &source) {
    destination.insert(source.begin(), source.end());
  }

  static void filterSources(SourceSet &sources, bool enabled,
                            std::initializer_list<ZeroOperation> operations) {
    if (enabled && contextIsZero(operations))
      sources.clear();
  }

  E1<D> sliceDerivative(const E1<D> &expression,
                        const std::vector<bool> &allowed_sources,
                        LinearSliceContext &context) const {
    if (!expression)
      return nullptr;
    auto cached = context.memo.find(expression.get());
    if (cached != context.memo.end())
      return cached->second;

    using K = typename Exp1<D>::K;
    E1<D> result;
    switch (expression->k) {
    case K::Term:
    case K::Bound:
      result = expression;
      break;
    case K::Seq: {
      E1<D> child = sliceDerivative(expression->t, allowed_sources, context);
      if (child)
        result = Exp1<D>::seq(expression->c, std::move(child));
      break;
    }
    case K::SeqR: {
      E1<D> child = sliceDerivative(expression->t, allowed_sources, context);
      if (child)
        result = Exp1<D>::seqR(std::move(child), expression->c);
      break;
    }
    case K::Call:
    case K::Hole:
      if (allowed_sources[sourceIndex(expression->sym)])
        result = expression;
      break;
    case K::Cond:
      result =
          sliceDerivative(expression->phi ? expression->t1 : expression->t2,
                          allowed_sources, context);
      break;
    case K::Ndet:
    case K::Add: {
      E1<D> lhs = sliceDerivative(expression->t1, allowed_sources, context);
      E1<D> rhs = sliceDerivative(expression->t2, allowed_sources, context);
      result = combineLinear(std::move(lhs), std::move(rhs));
      break;
    }
    case K::Sub: {
      E1<D> lhs = sliceDerivative(expression->t1, allowed_sources, context);
      E1<D> rhs = sliceDerivative(expression->t2, allowed_sources, context);
      if (lhs || rhs) {
        if (!lhs)
          lhs = Exp1<D>::term(D::zero());
        if (!rhs)
          rhs = Exp1<D>::term(D::zero());
        result = Exp1<D>::sub(std::move(lhs), std::move(rhs));
      }
      break;
    }
    case K::Project: {
      E1<D> child = sliceDerivative(expression->t, allowed_sources, context);
      if (child)
        result = Exp1<D>::project(std::move(child));
      break;
    }
    case K::Concat:
      if (allowed_sources[sourceIndex(expression->sym)])
        result = expression;
      break;
    case K::Star:
    case K::Mu: {
      E1<D> child = sliceDerivative(expression->t, allowed_sources, context);
      if (child) {
        result = expression->k == K::Star
                     ? Exp1<D>::star(std::move(child), expression->sym)
                     : Exp1<D>::mu(std::move(child), expression->sym);
      }
      break;
    }
    }
    context.memo.emplace(expression.get(), result);
    return result;
  }

  SharedDerivative differentiateExpression(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      DerivativeBuildContext &context, std::size_t scope) const {
    const NodeScopeKey key{expression.get(), scope};
    auto cached = context.memo.find(key);
    if (cached != context.memo.end())
      return cached->second;

    SharedDerivative result = differentiateExpressionUncached(
        expression, nu, env, bound, context, scope);
    context.memo.emplace(key, result);
    return result;
  }

  SharedDerivative differentiateExpressionUncached(
      const E0<D> &expression, const std::unordered_map<Symbol, V> &nu,
      const Env &env, const std::unordered_set<Symbol> &bound,
      DerivativeBuildContext &context, std::size_t scope) const {
    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Term:
      return std::make_shared<DerivativeResult>(
          DerivativeResult{expression->c, {}, nullptr});
    case K::Seq: {
      SharedDerivative child = differentiateExpression(expression->t, nu, env,
                                                       bound, context, scope);
      SourceSet sources = child->sources;
      filterSources(sources, context.prune_zero, {left(expression->c)});
      E1<D> derivative = child->derivative
                             ? Exp1<D>::seq(expression->c, child->derivative)
                             : nullptr;
      return std::make_shared<DerivativeResult>(
          DerivativeResult{D::extend(expression->c, child->value),
                           std::move(sources), std::move(derivative)});
    }
    case K::Mul: {
      SharedDerivative lhs = differentiateExpression(expression->t1, nu, env,
                                                     bound, context, scope);
      SharedDerivative rhs = differentiateExpression(expression->t2, nu, env,
                                                     bound, context, scope);
      SourceSet sources = lhs->sources;
      filterSources(sources, context.prune_zero, {right(rhs->value)});
      SourceSet rhs_sources = rhs->sources;
      filterSources(rhs_sources, context.prune_zero, {left(lhs->value)});
      mergeSources(sources, rhs_sources);
      E1<D> lhs_term = lhs->derivative
                           ? Exp1<D>::seqR(lhs->derivative, rhs->value)
                           : nullptr;
      E1<D> rhs_term =
          rhs->derivative ? Exp1<D>::seq(lhs->value, rhs->derivative) : nullptr;
      return std::make_shared<DerivativeResult>(DerivativeResult{
          D::extend(lhs->value, rhs->value), std::move(sources),
          combineLinear(std::move(lhs_term), std::move(rhs_term))});
    }
    case K::Call: {
      SharedDerivative argument = differentiateExpression(
          expression->t, nu, env, bound, context, scope);
      const V &callee = nu.at(expression->sym);
      SourceSet sources = argument->sources;
      filterSources(sources, context.prune_zero, {left(callee)});
      noteLeaf(expression->sym, context);
      if (!context.prune_zero || !contextIsZero({right(argument->value)}))
        sources.insert(sourceIndex(expression->sym));
      E1<D> argument_term = argument->derivative
                                ? Exp1<D>::seq(callee, argument->derivative)
                                : nullptr;
      E1<D> callee_term = Exp1<D>::call(expression->sym, argument->value);
      return std::make_shared<DerivativeResult>(DerivativeResult{
          D::extend(callee, argument->value), std::move(sources),
          combineLinear(std::move(argument_term), std::move(callee_term))});
    }
    case K::Cond: {
      SharedDerivative then_result = differentiateExpression(
          expression->t1, nu, env, bound, context, scope);
      SharedDerivative else_result = differentiateExpression(
          expression->t2, nu, env, bound, context, scope);
      SourceSet sources =
          expression->phi ? then_result->sources : else_result->sources;
      E1<D> derivative =
          expression->phi ? then_result->derivative : else_result->derivative;
      return std::make_shared<DerivativeResult>(
          DerivativeResult{D::condCombine(expression->phi, then_result->value,
                                          else_result->value),
                           std::move(sources), std::move(derivative)});
    }
    case K::Ndet: {
      SharedDerivative lhs = differentiateExpression(expression->t1, nu, env,
                                                     bound, context, scope);
      SharedDerivative rhs = differentiateExpression(expression->t2, nu, env,
                                                     bound, context, scope);
      SourceSet sources = lhs->sources;
      mergeSources(sources, rhs->sources);
      return std::make_shared<DerivativeResult>(DerivativeResult{
          D::ndetCombine(lhs->value, rhs->value), std::move(sources),
          combineLinear(lhs->derivative, rhs->derivative)});
    }
    case K::Project: {
      SharedDerivative child = differentiateExpression(expression->t, nu, env,
                                                       bound, context, scope);
      E1<D> derivative =
          child->derivative ? Exp1<D>::project(child->derivative) : nullptr;
      return std::make_shared<DerivativeResult>(
          DerivativeResult{domain_project<D>(child->value), child->sources,
                           std::move(derivative)});
    }
    case K::Hole: {
      noteLeaf(expression->sym, context);
      const unsigned source = sourceIndex(expression->sym);
      return std::make_shared<DerivativeResult>(DerivativeResult{
          nu.at(expression->sym), {source}, Exp1<D>::hole(expression->sym)});
    }
    case K::Bound:
      return std::make_shared<DerivativeResult>(
          DerivativeResult{env.at(expression->sym), {}, nullptr});
    case K::Concat: {
      SharedDerivative lhs = differentiateExpression(expression->t1, nu, env,
                                                     bound, context, scope);
      SharedDerivative rhs = differentiateExpression(expression->t2, nu, env,
                                                     bound, context, scope);
      auto local = env.find(expression->sym);
      const V &middle =
          local == env.end() ? nu.at(expression->sym) : local->second;

      const V middle_right = D::extend(middle, rhs->value);
      SourceSet sources = lhs->sources;
      filterSources(sources, context.prune_zero, {right(middle_right)});
      SourceSet rhs_sources = rhs->sources;
      filterSources(rhs_sources, context.prune_zero,
                    {left(middle), left(lhs->value)});
      mergeSources(sources, rhs_sources);

      if (!bound.count(expression->sym)) {
        noteLeaf(expression->sym, context);
        if (!context.prune_zero ||
            !contextIsZero({left(lhs->value), right(rhs->value)}))
          sources.insert(sourceIndex(expression->sym));
      }

      E1<D> lhs_term = lhs->derivative
                           ? Exp1<D>::seqR(lhs->derivative, middle_right)
                           : nullptr;
      E1<D> middle_term;
      if (!bound.count(expression->sym)) {
        middle_term =
            Exp1<D>::concat(Exp1<D>::term(lhs->value), expression->sym,
                            Exp1<D>::term(rhs->value));
      }
      E1<D> rhs_term =
          rhs->derivative
              ? Exp1<D>::seq(lhs->value, Exp1<D>::seq(middle, rhs->derivative))
              : nullptr;
      E1<D> derivative = combineLinear(
          combineLinear(std::move(lhs_term), std::move(middle_term)),
          std::move(rhs_term));
      return std::make_shared<DerivativeResult>(
          DerivativeResult{D::extend(lhs->value, middle_right),
                           std::move(sources), std::move(derivative)});
    }
    case K::Star: {
      if constexpr (DomainHasStar<D>::value) {
        if (E0<D> operand = matchSemiringStarOperand<D>(expression)) {
          SharedDerivative body =
              differentiateExpression(operand, nu, env, bound, context, scope);
          V star_value = D::star(body->value);
          SourceSet sources = body->sources;
          filterSources(sources, context.prune_zero,
                        {right(star_value), left(star_value)});
          E1<D> derivative =
              body->derivative
                  ? Exp1<D>::seq(star_value,
                                 Exp1<D>::seqR(body->derivative, star_value))
                  : nullptr;
          return std::make_shared<DerivativeResult>(
              DerivativeResult{std::move(star_value), std::move(sources),
                               std::move(derivative)});
        }
      }

      V star_value = I0<D>::evalWithEnvironment(nu, env, expression);
      Env body_env = env;
      body_env.insert_or_assign(expression->sym, star_value);
      auto body_bound = bound;
      body_bound.insert(expression->sym);
      const std::size_t body_scope = context.next_scope++;
      SharedDerivative body = differentiateExpression(
          expression->t, nu, body_env, body_bound, context, body_scope);
      SourceSet sources = body->sources;
      filterSources(sources, context.prune_zero,
                    {right(star_value), left(star_value)});
      E1<D> derivative =
          body->derivative
              ? Exp1<D>::seq(star_value,
                             Exp1<D>::seqR(body->derivative, star_value))
              : nullptr;
      return std::make_shared<DerivativeResult>(DerivativeResult{
          std::move(star_value), std::move(sources), std::move(derivative)});
    }
    case K::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }
};

} // namespace detail
} // namespace npa

