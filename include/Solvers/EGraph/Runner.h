#pragma once

#include "Solvers/EGraph/Explain.h"
#include "Solvers/EGraph/Extract.h"
#include "Solvers/EGraph/Rewrite.h"

namespace lotus::egraph {

enum class StopReasonKind {
  None,
  Saturated,
  IterationLimit,
  NodeLimit,
  TimeLimit,
  Other,
};

struct StopReason {
  StopReasonKind kind = StopReasonKind::None;
  std::string detail;
};

struct RunnerLimits {
  size_t iter_limit = 30;
  size_t node_limit = 10'000;
  Duration time_limit = std::chrono::seconds(5);
  std::optional<Instant> start_time;

  template <typename L, typename A>
  std::optional<StopReason> check(size_t iteration,
                                  const EGraph<L, A> &egraph) const {
    if (start_time && now() - *start_time > time_limit) {
      return StopReason{StopReasonKind::TimeLimit, "time limit exceeded"};
    }
    if (egraph.totalSize() > node_limit) {
      return StopReason{StopReasonKind::NodeLimit,
                        std::to_string(egraph.totalSize())};
    }
    if (iteration >= iter_limit) {
      return StopReason{StopReasonKind::IterationLimit,
                        std::to_string(iteration)};
    }
    return std::nullopt;
  }
};

template <typename IterData> struct Iteration {
  size_t egraph_nodes = 0;
  size_t egraph_classes = 0;
  std::unordered_map<std::string, size_t> applied;
  double hook_time = 0.0;
  double search_time = 0.0;
  double apply_time = 0.0;
  double rebuild_time = 0.0;
  double total_time = 0.0;
  IterData data{};
  size_t n_rebuilds = 0;
  std::optional<StopReason> stop_reason;
};

struct Report {
  size_t iterations = 0;
  StopReason stop_reason;
  size_t egraph_nodes = 0;
  size_t egraph_classes = 0;
  size_t memo_size = 0;
  size_t rebuilds = 0;
  double total_time = 0.0;
  double search_time = 0.0;
  double apply_time = 0.0;
  double rebuild_time = 0.0;
};

template <typename L, typename A = NoAnalysis<L>,
          typename IterData = std::monostate>
class Runner;

template <typename L, typename A, typename IterData> struct IterationData {
  static IterData make(const Runner<L, A, IterData> &) { return IterData{}; }
};

template <typename IterData>
inline std::ostream &operator<<(std::ostream &os,
                                const Iteration<IterData> &iteration) {
  os << "Iteration(nodes=" << iteration.egraph_nodes
     << ", classes=" << iteration.egraph_classes << ")";
  return os;
}

inline std::ostream &operator<<(std::ostream &os, const Report &report) {
  os << "Runner report\n"
     << "  Stop reason: " << static_cast<int>(report.stop_reason.kind) << "\n"
     << "  Iterations: " << report.iterations << "\n"
     << "  Egraph size: " << report.egraph_nodes << " nodes, "
     << report.egraph_classes << " classes, " << report.memo_size << " memo\n"
     << "  Rebuilds: " << report.rebuilds << "\n"
     << "  Total time: " << report.total_time << "\n"
     << "  Search time: " << report.search_time << "\n"
     << "  Apply time: " << report.apply_time << "\n"
     << "  Rebuild time: " << report.rebuild_time;
  return os;
}

template <typename L, typename A> class RewriteScheduler {
public:
  virtual ~RewriteScheduler() = default;

  virtual bool canStop(size_t) { return true; }

  virtual std::vector<SearchMatches<L>>
  searchRewrite(size_t, const EGraph<L, A> &egraph,
                const Rewrite<L, A> &rewrite) {
    return rewrite.search(egraph);
  }

  virtual std::vector<std::vector<SearchMatches<L>>>
  searchRewrites(size_t iteration, const EGraph<L, A> &egraph,
                 const std::vector<Rewrite<L, A>> &rewrites,
                 const RunnerLimits &limits) {
    std::vector<std::vector<SearchMatches<L>>> out;
    out.reserve(rewrites.size());
    for (const auto &rewrite : rewrites) {
      out.push_back(searchRewrite(iteration, egraph, rewrite));
      if (limits.check(iteration, egraph)) {
        break;
      }
    }
    return out;
  }

  virtual size_t applyRewrite(size_t, EGraph<L, A> &egraph,
                              const Rewrite<L, A> &rewrite,
                              std::vector<SearchMatches<L>> matches) {
    return rewrite.apply(egraph, matches).size();
  }
};

template <typename L, typename A>
class SimpleScheduler final : public RewriteScheduler<L, A> {};

template <typename L, typename A>
class BackoffScheduler final : public RewriteScheduler<L, A> {
public:
  BackoffScheduler &withInitialMatchLimit(size_t limit) {
    default_match_limit_ = limit;
    return *this;
  }

  BackoffScheduler &withBanLength(size_t length) {
    default_ban_length_ = length;
    return *this;
  }

  BackoffScheduler &doNotBan(std::string name) {
    auto &stats = ruleStats(std::move(name));
    stats.match_limit = std::numeric_limits<size_t>::max();
    return *this;
  }

  BackoffScheduler &ruleMatchLimit(std::string name, size_t limit) {
    ruleStats(std::move(name)).match_limit = limit;
    return *this;
  }

  BackoffScheduler &ruleBanLength(std::string name, size_t length) {
    ruleStats(std::move(name)).ban_length = length;
    return *this;
  }

  bool canStop(size_t iteration) override {
    bool any_banned = false;
    size_t min_banned_until = std::numeric_limits<size_t>::max();
    for (const auto &[_, stats] : stats_) {
      if (stats.banned_until > iteration) {
        any_banned = true;
        min_banned_until = std::min(min_banned_until, stats.banned_until);
      }
    }
    if (!any_banned) {
      return true;
    }

    size_t delta = min_banned_until - iteration;
    for (auto &[_, stats] : stats_) {
      if (stats.banned_until > iteration) {
        stats.banned_until -= delta;
      }
    }
    return false;
  }

  std::vector<SearchMatches<L>>
  searchRewrite(size_t iteration, const EGraph<L, A> &egraph,
                const Rewrite<L, A> &rewrite) override {
    auto &stats = ruleStats(rewrite.name());
    if (iteration < stats.banned_until) {
      return {};
    }

    size_t threshold = stats.match_limit;
    if (stats.times_banned < sizeof(size_t) * 8) {
      threshold = threshold << stats.times_banned;
    }
    auto matches = rewrite.searchWithLimit(egraph, threshold + 1);
    size_t total = 0;
    for (const auto &match : matches) {
      total += match.substs.size();
    }
    if (total > threshold) {
      size_t ban_length = stats.ban_length;
      if (stats.times_banned < sizeof(size_t) * 8) {
        ban_length <<= stats.times_banned;
      }
      ++stats.times_banned;
      stats.banned_until = iteration + ban_length;
      return {};
    }

    ++stats.times_applied;
    return matches;
  }

private:
  struct RuleStats {
    size_t times_applied = 0;
    size_t banned_until = 0;
    size_t times_banned = 0;
    size_t match_limit = 1000;
    size_t ban_length = 5;
  };

  RuleStats &ruleStats(std::string name) {
    auto [it, inserted] = stats_.try_emplace(std::move(name));
    if (inserted) {
      it->second.match_limit = default_match_limit_;
      it->second.ban_length = default_ban_length_;
    }
    return it->second;
  }

  size_t default_match_limit_ = 1000;
  size_t default_ban_length_ = 5;
  std::unordered_map<std::string, RuleStats> stats_;
};

template <typename L, typename A, typename IterData> class Runner {
public:
  explicit Runner(A analysis = A())
      : egraph(std::move(analysis)),
        scheduler_(std::make_unique<BackoffScheduler<L, A>>()) {}

  Runner &withExpr(const RecExpr<L> &expr) {
    roots.push_back(egraph.addExpr(expr));
    return *this;
  }

  Runner &withEGraph(EGraph<L, A> graph) {
    egraph = std::move(graph);
    return *this;
  }

  Runner &withIterLimit(size_t limit) {
    limits_.iter_limit = limit;
    return *this;
  }

  Runner &withNodeLimit(size_t limit) {
    limits_.node_limit = limit;
    return *this;
  }

  Runner &withTimeLimit(Duration limit) {
    limits_.time_limit = limit;
    return *this;
  }

  Runner &withHook(std::function<std::optional<std::string>(Runner &)> hook) {
    hooks.push_back(std::move(hook));
    return *this;
  }

  Runner &withExplanationsEnabled() {
    egraph = egraph.withExplanationsEnabled();
    return *this;
  }

  Runner &withExplanationsDisabled() {
    egraph = egraph.withExplanationsDisabled();
    return *this;
  }

  Runner &withoutExplanationLengthOptimization() {
    egraph = egraph.withoutExplanationLengthOptimization();
    return *this;
  }

  Runner &withExplanationLengthOptimization() {
    egraph = egraph.withExplanationLengthOptimization();
    return *this;
  }

  Runner &withScheduler(std::unique_ptr<RewriteScheduler<L, A>> scheduler) {
    scheduler_ = std::move(scheduler);
    return *this;
  }

  Runner &withScheduler(SimpleScheduler<L, A> scheduler) {
    scheduler_ = std::make_unique<SimpleScheduler<L, A>>(std::move(scheduler));
    return *this;
  }

  Runner &withScheduler(BackoffScheduler<L, A> scheduler) {
    scheduler_ = std::make_unique<BackoffScheduler<L, A>>(std::move(scheduler));
    return *this;
  }

  Runner &run(const std::vector<Rewrite<L, A>> &rules) {
    checkRules(rules);
    egraph.rebuild();
    limits_.start_time = now();
    stop_reason = {};
    iterations.clear();

    for (size_t i = 0;; ++i) {
      auto iteration = runOne(i, rules);
      auto maybe_stop = iteration.stop_reason;
      iterations.push_back(iteration);
      if (maybe_stop) {
        stop_reason = *maybe_stop;
        break;
      }
      if (auto limit_stop = limits_.check(i + 1, egraph)) {
        stop_reason = *limit_stop;
        iterations.back().stop_reason = *limit_stop;
        break;
      }
    }
    return *this;
  }

  Report report() const {
    Report out;
    out.iterations = iterations.size();
    out.stop_reason = stop_reason;
    out.egraph_nodes = egraph.totalNumberOfNodes();
    out.egraph_classes = egraph.numberOfClasses();
    out.memo_size = egraph.totalSize();
    for (const auto &iteration : iterations) {
      out.rebuilds += iteration.n_rebuilds;
      out.total_time += iteration.total_time;
      out.search_time += iteration.search_time;
      out.apply_time += iteration.apply_time;
      out.rebuild_time += iteration.rebuild_time;
    }
    return out;
  }

  void printReport(std::ostream &os = std::cout) const {
    os << report() << '\n';
  }

  std::optional<Explanation<L>> explainEquivalence(const RecExpr<L> &lhs,
                                                   const RecExpr<L> &rhs) {
    return lotus::egraph::explainEquivalence(egraph, lhs, rhs);
  }

  std::optional<Explanation<L>> explainMatches(const RecExpr<L> &lhs,
                                               const PatternAst<L> &rhs,
                                               const Subst &subst) {
    return lotus::egraph::explainMatches(egraph, lhs, rhs, subst);
  }

  EGraph<L, A> egraph;
  std::vector<Iteration<IterData>> iterations;
  std::vector<Id> roots;
  StopReason stop_reason;
  std::vector<std::function<std::optional<std::string>(Runner &)>> hooks;

private:
  void checkRules(const std::vector<Rewrite<L, A>> &rules) const {
    std::unordered_map<std::string, size_t> counts;
    for (const auto &rule : rules) {
      ++counts[rule.name()];
    }
    for (const auto &[name, count] : counts) {
      if (count > 1) {
        std::cerr << "WARNING: rule '" << name << "' appears " << count
                  << " times; scheduling and reporting may be affected\n";
      }
    }
  }

  Iteration<IterData> runOne(size_t iteration_index,
                             const std::vector<Rewrite<L, A>> &rules) {
    Iteration<IterData> iteration;
    iteration.egraph_nodes = egraph.totalSize();
    iteration.egraph_classes = egraph.numberOfClasses();

    Instant iteration_start = now();

    auto limit_stop = limits_.check(iteration_index, egraph);
    if (limit_stop) {
      iteration.stop_reason = *limit_stop;
      return iteration;
    }

    Instant hook_start = now();
    for (auto &hook : hooks) {
      if (auto stop = hook(*this)) {
        iteration.hook_time =
            std::chrono::duration<double>(now() - hook_start).count();
        iteration.total_time =
            std::chrono::duration<double>(now() - iteration_start).count();
        iteration.stop_reason = StopReason{StopReasonKind::Other, *stop};
        return iteration;
      }
    }
    iteration.hook_time =
        std::chrono::duration<double>(now() - hook_start).count();

    size_t egraph_nodes_after_hooks = egraph.totalSize();
    size_t egraph_classes_after_hooks = egraph.numberOfClasses();

    Instant search_start = now();
    auto matches =
        scheduler_->searchRewrites(iteration_index, egraph, rules, limits_);
    iteration.search_time =
        std::chrono::duration<double>(now() - search_start).count();

    Instant apply_start = now();
    for (size_t i = 0; i < matches.size(); ++i) {
      size_t applied = scheduler_->applyRewrite(
          iteration_index, egraph, rules[i], std::move(matches[i]));
      if (applied > 0) {
        iteration.applied[rules[i].name()] += applied;
      }
      if (auto stop = limits_.check(iteration_index, egraph)) {
        iteration.stop_reason = *stop;
        break;
      }
    }
    iteration.apply_time =
        std::chrono::duration<double>(now() - apply_start).count();

    Instant rebuild_start = now();
    iteration.n_rebuilds = egraph.rebuild();
    iteration.rebuild_time =
        std::chrono::duration<double>(now() - rebuild_start).count();

    bool can_be_saturated =
        iteration.applied.empty() && scheduler_->canStop(iteration_index) &&
        iteration.egraph_nodes == egraph_nodes_after_hooks &&
        iteration.egraph_classes == egraph_classes_after_hooks &&
        iteration.egraph_nodes == egraph.totalSize() &&
        iteration.egraph_classes == egraph.numberOfClasses();
    if (can_be_saturated && !iteration.stop_reason) {
      iteration.stop_reason = StopReason{StopReasonKind::Saturated, {}};
    }

    iteration.data = IterationData<L, A, IterData>::make(*this);
    iteration.total_time =
        std::chrono::duration<double>(now() - iteration_start).count();
    return iteration;
  }

  RunnerLimits limits_;
  std::unique_ptr<RewriteScheduler<L, A>> scheduler_;
};

} // namespace lotus::egraph
